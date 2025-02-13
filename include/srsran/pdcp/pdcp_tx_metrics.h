/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "srsran/support/engineering_notation.h"
#include "srsran/support/format/fmt_to_c_str.h"
#include "srsran/support/timers.h"
#include "fmt/format.h"
#include "fmt/ranges.h"
#include <array>

/*
 * This file will hold the interfaces and structures for the
 * PDCP TX entity metrics collection. This also includes formatting
 * helpers for printing the metrics.
 */
namespace srsran {

/// This struct will hold relevant metrics for the PDCP TX
struct pdcp_tx_metrics_container {
  uint32_t num_sdus;
  uint32_t num_sdu_bytes;
  uint32_t num_pdus;
  uint32_t num_pdu_bytes;
  uint32_t num_discard_timeouts;
  uint32_t sum_pdu_latency_ns; ///< total PDU latency (in ns)
  unsigned counter;

  // CPU Usage metrics
  uint32_t sum_crypto_processing_latency_ns;

  // Histogram of PDU latencies
  static constexpr unsigned                   pdu_latency_hist_bins = 8;
  static constexpr unsigned                   nof_usec_per_bin      = 1;
  std::array<uint32_t, pdu_latency_hist_bins> pdu_latency_hist;
  uint32_t                                    max_pdu_latency_ns;
};

inline std::string format_pdcp_tx_metrics(timer_duration metrics_period, const pdcp_tx_metrics_container& m)
{
  fmt::memory_buffer buffer;
  fmt::format_to(std::back_inserter(buffer),
                 "num_sdus={} sdu_rate={}bps num_pdus={} pdu_rate={}bps num_discard_timeouts={} sum_sdu_latency={}ns "
                 "sdu_latency_hist=[",
                 scaled_fmt_integer(m.num_sdus, false),
                 float_to_eng_string(static_cast<float>(m.num_sdu_bytes) * 8 * 1000 / metrics_period.count(), 1, false),
                 scaled_fmt_integer(m.num_pdus, false),
                 float_to_eng_string(static_cast<float>(m.num_pdu_bytes) * 8 * 1000 / metrics_period.count(), 1, false),
                 scaled_fmt_integer(m.num_discard_timeouts, false),
                 m.sum_pdu_latency_ns);
  bool first_bin = true;
  for (auto freq : m.pdu_latency_hist) {
    fmt::format_to(std::back_inserter(buffer), "{}{}", first_bin ? "" : " ", float_to_eng_string(freq, 1, false));
    first_bin = false;
  }
  fmt::format_to(std::back_inserter(buffer),
                 "] max_pdu_latency={}us crypto_cpu_usage={}\%",
                 m.max_pdu_latency_ns * 1e-3,
                 static_cast<float>(m.sum_crypto_processing_latency_ns) / (1000000 * metrics_period.count()) * 100);
  return to_c_str(buffer);
}

} // namespace srsran

namespace fmt {
// PDCP TX metrics formatter
template <>
struct formatter<srsran::pdcp_tx_metrics_container> {
  template <typename ParseContext>
  auto parse(ParseContext& ctx)
  {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(srsran::pdcp_tx_metrics_container m, FormatContext& ctx) const
  {
    return format_to(ctx.out(),
                     "num_sdus={} num_sdu_bytes={} num_pdus={} num_pdu_bytes={} num_discard_timeouts={} "
                     "sum_pdu_latency={}ns sdu_latency_hist=[{}] max_sdu_latency={}ns sum_crypto_latency={}ns",
                     m.num_sdus,
                     m.num_sdu_bytes,
                     m.num_pdus,
                     m.num_pdu_bytes,
                     m.num_discard_timeouts,
                     m.sum_pdu_latency_ns,
                     fmt::join(m.pdu_latency_hist, " "),
                     m.max_pdu_latency_ns,
                     m.sum_crypto_processing_latency_ns);
  }
};
} // namespace fmt
