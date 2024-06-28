/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "du_low_config_yaml_writer.h"
#include "du_low_config.h"

using namespace srsran;

static void fill_du_low_log_section(YAML::Node node, const du_low_unit_logger_config& config)
{
  node["phy_level"]            = srslog::basic_level_to_string(config.phy_level);
  node["hex_max_size"]         = config.hex_max_size;
  node["broadcast_enabled"]    = config.broadcast_enabled;
  node["phy_rx_symbols_prach"] = config.phy_rx_symbols_prach;
  if (!config.phy_rx_symbols_filename.empty()) {
    node["phy_rx_symbols_filename"] = config.phy_rx_symbols_filename;
  }

  if (config.phy_rx_symbols_port.has_value()) {
    node["phy_rx_symbols_port"] = config.phy_rx_symbols_port.value();
  }
}

static void fill_du_low_expert_execution_section(YAML::Node node, const du_low_unit_expert_execution_config& config)
{
  {
    YAML::Node threads_node                 = node["threads"];
    YAML::Node upper_node                   = threads_node["upper_phy"];
    upper_node["pdsch_processor_type"]      = config.threads.pdsch_processor_type;
    upper_node["nof_pusch_decoder_threads"] = config.threads.nof_pusch_decoder_threads;
    upper_node["nof_ul_threads"]            = config.threads.nof_ul_threads;
    upper_node["nof_dl_threads"]            = config.threads.nof_dl_threads;
  }

  auto cell_affinities_node = node["cell_affinities"];
  while (config.cell_affinities.size() > cell_affinities_node.size()) {
    cell_affinities_node.push_back(YAML::Node());
  }

  unsigned index = 0;
  for (auto cell : cell_affinities_node) {
    const auto& expert = config.cell_affinities[index];

    if (expert.l1_dl_cpu_cfg.mask.any()) {
      cell["l1_dl_cpus"] = fmt::format("{:,}", span<const size_t>(expert.l1_dl_cpu_cfg.mask.get_cpu_ids()));
    }
    cell["l1_dl_pinning"] = to_string(expert.l1_dl_cpu_cfg.pinning_policy);

    if (expert.l1_ul_cpu_cfg.mask.any()) {
      cell["l1_ul_cpus"] = fmt::format("{:,}", span<const size_t>(expert.l1_ul_cpu_cfg.mask.get_cpu_ids()));
    }
    cell["l1_ul_pinning"] = to_string(expert.l1_ul_cpu_cfg.pinning_policy);

    ++index;
  }
}

static void fill_du_low_expert_section(YAML::Node node, const du_low_unit_expert_upper_phy_config& config)
{
  node["max_proc_delay"]              = config.max_processing_delay_slots;
  node["pusch_dec_max_iterations"]    = config.pusch_decoder_max_iterations;
  node["pusch_dec_enable_early_stop"] = config.pusch_decoder_early_stop;
  node["pusch_sinr_calc_method"]      = config.pusch_sinr_calc_method;
  node["max_request_headroom_slots"]  = config.nof_slots_request_headroom;
}

void srsran::fill_du_low_config_in_yaml_schema(YAML::Node& node, const du_low_unit_config& config)
{
  fill_du_low_log_section(node["log"], config.loggers);
  fill_du_low_expert_execution_section(node["expert_execution"], config.expert_execution_cfg);
  fill_du_low_expert_section(node["expert_phy"], config.expert_phy_cfg);
}
