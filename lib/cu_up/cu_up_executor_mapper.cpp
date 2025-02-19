/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srsran/cu_up/cu_up_executor_mapper.h"
#include "srsran/adt/mpmc_queue.h"
#include "srsran/support/async/execute_on_blocking.h"
#include "srsran/support/executors/inline_task_executor.h"
#include "srsran/support/executors/strand_executor.h"
#include <variant>

using namespace srsran;
using namespace srs_cu_up;

namespace {

/// Task executor adaptor that allows cancelling pending tasks, from within the executor's context.
class cancellable_task_executor final : public task_executor
{
public:
  cancellable_task_executor(task_executor& exec_, const std::atomic<bool>& cancelled_flag, timer_manager& timers_) :
    exec(&exec_), cancelled(cancelled_flag), timers(timers_)
  {
  }

  ~cancellable_task_executor() override
  {
    if (!cancelled) {
      logger.error("cancellable_task_executor destroyed before tasks being cancelled");
    }
  }

  [[nodiscard]] bool execute(unique_task task) override
  {
    return exec->execute([this, task = std::move(task)]() {
      if (cancelled.load(std::memory_order_acquire)) {
        return;
      }
      task();
    });
  }

  [[nodiscard]] bool defer(unique_task task) override
  {
    return exec->defer([this, task = std::move(task)]() {
      if (cancelled.load(std::memory_order_acquire)) {
        return;
      }
      task();
    });
  }

  auto defer_on()
  {
    // We use the underlying executor to ignore cancelled flag.
    return defer_on_blocking(*exec, timers);
  }

private:
  task_executor*           exec;
  const std::atomic<bool>& cancelled;
  timer_manager&           timers;

  // logger
  srslog::basic_logger& logger = srslog::fetch_basic_logger("CU-UP", false);
};

/// Implementation of the UE executor mapper.
class ue_executor_mapper_impl final : public ue_executor_mapper
{
public:
  ue_executor_mapper_impl(task_executor& ctrl_exec_,
                          task_executor& ul_exec_,
                          task_executor& dl_exec_,
                          task_executor& crypto_exec_,
                          timer_manager& timers_) :
    timers(timers_),
    ctrl_exec(ctrl_exec_, cancelled_flag, timers),
    ul_exec(ul_exec_, cancelled_flag, timers),
    dl_exec(dl_exec_, cancelled_flag, timers),
    crypto_exec(crypto_exec_)
  {
  }

  ~ue_executor_mapper_impl() override
  {
    if (!cancelled_flag.load(std::memory_order_relaxed)) {
      logger.error("ue_executor_mapper_impl destroyed before tasks being cancelled");
    }
  }

  async_task<void> stop() override
  {
    return launch_async([this](coro_context<async_task<void>>& ctx) mutable {
      CORO_BEGIN(ctx);

      if (cancel_tasks()) {
        // Await for tasks for the given UE to be completely flushed, before proceeding.
        // TODO: Use when_all.
        CORO_AWAIT(dl_exec.defer_on());
        CORO_AWAIT(ul_exec.defer_on());
        // Revert back to ctrl exec.
        CORO_AWAIT(ctrl_exec.defer_on());
      }

      CORO_RETURN();
    });
  }

  task_executor& ctrl_executor() override { return ctrl_exec; }
  task_executor& ul_pdu_executor() override { return ul_exec; }
  task_executor& dl_pdu_executor() override { return dl_exec; }
  task_executor& crypto_executor() override { return crypto_exec; }

private:
  bool cancel_tasks() { return not cancelled_flag.exchange(true, std::memory_order_acq_rel); }

  std::atomic<bool>         cancelled_flag{false};
  timer_manager&            timers;
  cancellable_task_executor ctrl_exec;
  cancellable_task_executor ul_exec;
  cancellable_task_executor dl_exec;
  task_executor&            crypto_exec;

  // logger
  srslog::basic_logger& logger = srslog::fetch_basic_logger("CU-UP", false);
};

struct base_cu_up_executor_pool_config {
  task_executor&       main_exec;
  span<task_executor*> dl_executors;
  span<task_executor*> ul_executors;
  span<task_executor*> ctrl_executors;
  task_executor&       crypto_exec;
  timer_manager&       timers;
};

class round_robin_cu_up_exec_pool
{
public:
  round_robin_cu_up_exec_pool(base_cu_up_executor_pool_config config) : timers(config.timers)
  {
    srsran_assert(config.ctrl_executors.size() > 0, "At least one DL executor must be specified");
    if (config.dl_executors.empty()) {
      config.dl_executors = config.ctrl_executors;
    } else {
      srsran_assert(config.dl_executors.size() == config.ctrl_executors.size(),
                    "If specified, the number of DL executors must be equal to the number of control executors");
    }
    if (config.ul_executors.empty()) {
      config.ul_executors = config.ctrl_executors;
    } else {
      srsran_assert(config.ul_executors.size() == config.ctrl_executors.size(),
                    "If specified, the number of UL executors must be equal to the number of control executors");
    }

    for (unsigned i = 0; i != config.ctrl_executors.size(); ++i) {
      execs.emplace_back(
          *config.ctrl_executors[i], *config.ul_executors[i], *config.dl_executors[i], config.crypto_exec);
    }
  }

  std::unique_ptr<ue_executor_mapper> create_ue_executor_mapper()
  {
    auto& ctxt = execs[round_robin_index.fetch_add(1, std::memory_order_relaxed) % execs.size()];
    return std::make_unique<ue_executor_mapper_impl>(
        ctxt.ctrl_exec, ctxt.ul_exec, ctxt.dl_exec, ctxt.crypto_exec, timers);
  }

private:
  struct ue_executor_context {
    task_executor& ctrl_exec;
    task_executor& ul_exec;
    task_executor& dl_exec;
    task_executor& crypto_exec;

    ue_executor_context(task_executor& ctrl_exec_,
                        task_executor& ul_exec_,
                        task_executor& dl_exec_,
                        task_executor& crypto_exec_) :
      ctrl_exec(ctrl_exec_), ul_exec(ul_exec_), dl_exec(dl_exec_), crypto_exec(crypto_exec_)
    {
    }
  };

  // Main executor of the CU-UP.
  timer_manager& timers;

  // List of UE executor mapper contexts created.
  std::vector<ue_executor_context> execs;

  // A round-robin algorithm is used to distribute executors to UEs.
  std::atomic<uint32_t> round_robin_index{0};
};

/// \brief CU-UP Executor Pool based on strands pointing to a worker pool.
///
/// This is the executor architecture:
/// - "main_executor" is a strand called "cu_up_strand" that wraps "worker_pool_executor". Thus, it is sequential.
/// - "crypto_executor" is a pointer to "worker_pool_executor". Thus, it is *not* sequential.
/// - "ue_ctrl_executor", "ue_ul_executor", "ue_dl_executor" are the three-level priorities of a strand that adapts
/// the "main_executor" strand. "ue_ctrl_executor" is for timers and control events, "ue_ul_executor" for UL PDUs,
/// and "ue_dl_executor" for DL PDUs.
/// Thus, all executors, with the exception of "crypto_executor", go through the same "cu_up_strand", and there is
/// no parallelization, except for the crypto tasks.
/// TODO: Revisit executor architecture once CU-UP supports parallelization.
class strand_based_cu_up_executor_mapper final : public cu_up_executor_mapper
{
public:
  strand_based_cu_up_executor_mapper(const strand_based_executor_config& config) :
    cu_up_strand(&config.worker_pool_executor, config.default_task_queue_size), cu_up_exec_pool(create_strands(config))
  {
  }

  task_executor& ctrl_executor() override { return cu_up_strand; }

  task_executor& io_ul_executor() override { return *io_ul_exec_ptr; }

  task_executor& e2_executor() override { return cu_up_strand; }

  std::unique_ptr<ue_executor_mapper> create_ue_executor_mapper() override
  {
    return cu_up_exec_pool.create_ue_executor_mapper();
  }

private:
  using cu_up_strand_type        = task_strand<task_executor*, concurrent_queue_policy::lockfree_mpmc>;
  using io_dedicated_strand_type = task_strand<task_executor*, concurrent_queue_policy::lockfree_mpmc>;
  using ue_strand_type           = priority_task_strand<cu_up_strand_type*>;

  base_cu_up_executor_pool_config create_strands(const strand_based_executor_config& config)
  {
    concurrent_queue_params qparams{srsran::concurrent_queue_policy::lockfree_mpmc, config.default_task_queue_size};
    concurrent_queue_params data_qparams{srsran::concurrent_queue_policy::lockfree_mpmc, config.gtpu_task_queue_size};

    // Create IO executor that can be either inlined with CU-UP strand or its own strand.
    if (config.dedicated_io_strand) {
      io_ul_exec.emplace<io_dedicated_strand_type>(&config.worker_pool_executor, config.gtpu_task_queue_size);
      io_ul_exec_ptr = &std::get<io_dedicated_strand_type>(io_ul_exec);
    } else {
      io_ul_exec.emplace<inline_task_executor>();
      io_ul_exec_ptr = &std::get<inline_task_executor>(io_ul_exec);
    }

    // Create UE-dedicated strands.
    ue_strands.resize(config.max_nof_ue_strands);
    ue_ctrl_execs.resize(config.max_nof_ue_strands);
    ue_ul_execs.resize(config.max_nof_ue_strands);
    ue_dl_execs.resize(config.max_nof_ue_strands);
    std::array<concurrent_queue_params, 3> ue_queue_params = {qparams, data_qparams, data_qparams};
    for (unsigned i = 0; i != config.max_nof_ue_strands; ++i) {
      ue_strands[i]                             = std::make_unique<ue_strand_type>(&cu_up_strand, ue_queue_params);
      span<ue_strand_type::executor_type> execs = ue_strands[i]->get_executors();
      srsran_assert(execs.size() == 3, "Three executors should have been created for the three priorities");
      ue_ctrl_execs[i] = &execs[0];
      ue_ul_execs[i]   = &execs[1];
      ue_dl_execs[i]   = &execs[2];
    }

    return base_cu_up_executor_pool_config{
        cu_up_strand, ue_dl_execs, ue_ul_execs, ue_ctrl_execs, config.worker_pool_executor, *config.timers};
  }

  // Base strand that sequentializes accesses to the worker pool executor.
  cu_up_strand_type cu_up_strand;

  // IO executor with two modes
  std::variant<inline_task_executor, io_dedicated_strand_type> io_ul_exec;
  task_executor*                                               io_ul_exec_ptr;

  // UE strands and respective executors.
  std::vector<std::unique_ptr<ue_strand_type>> ue_strands;
  std::vector<task_executor*>                  ue_ctrl_execs;
  std::vector<task_executor*>                  ue_ul_execs;
  std::vector<task_executor*>                  ue_dl_execs;

  // pool of UE executors with round-robin dispatch policy.
  round_robin_cu_up_exec_pool cu_up_exec_pool;
};

} // namespace

std::unique_ptr<cu_up_executor_mapper>
srsran::srs_cu_up::make_cu_up_executor_mapper(const strand_based_executor_config& config)
{
  return std::make_unique<strand_based_cu_up_executor_mapper>(config);
}
