/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "ue_capability_manager.h"
#include "srsran/asn1/rrc_nr/ul_dcch_msg_ies.h"

using namespace srsran;
using namespace srs_du;

expected<ue_capability_summary, std::string>
srsran::srs_du::decode_ue_nr_cap_container(const byte_buffer& ue_cap_container)
{
  asn1::rrc_nr::ue_nr_cap_s ue_cap;
  {
    asn1::cbit_ref bref{ue_cap_container};
    if (ue_cap.unpack(bref) != asn1::SRSASN_SUCCESS) {
      return make_unexpected(std::string("Couldn't unpack UE NR Capability RRC container"));
    }
  }

  ue_capability_summary ue_caps;

  // fill UE capability summary.
  if (ue_cap.phy_params.phy_params_fr1_present) {
    ue_caps.pdsch_qam256_supported = ue_cap.phy_params.phy_params_fr1.pdsch_256_qam_fr1_present;
  }
  for (const auto& band : ue_cap.rf_params.supported_band_list_nr) {
    // Create and convert band capability.
    ue_capability_summary::supported_band band_cap;
    band_cap.pusch_qam256_supported = band.pusch_256_qam_present;

    // Emplace the band capability in the map.
    ue_caps.bands.emplace(static_cast<nr_band>(band.band_nr), band_cap);
  }
  if (ue_cap.mac_params_present and ue_cap.mac_params.mac_params_xdd_diff_present) {
    ue_caps.long_drx_cycle_supported  = ue_cap.mac_params.mac_params_xdd_diff.long_drx_cycle_present;
    ue_caps.short_drx_cycle_supported = ue_cap.mac_params.mac_params_xdd_diff.short_drx_cycle_present;
  }

  // Convert advanced UE NR capabilities.
  decode_advanced_ue_nr_caps(ue_caps, ue_cap);

  return ue_caps;
}

SRSRAN_WEAK_SYMB void srsran::srs_du::decode_advanced_ue_nr_caps(srsran::srs_du::ue_capability_summary& ue_capability,
                                                                 const asn1::rrc_nr::ue_nr_cap_s&       ue_caps)
{
  // Advanced UE capabilities is not implemented.
}

// Configure dedicated UE configuration to set MCS ant CQI tables.
static void set_pdsch_mcs_table(serving_cell_config& cell_cfg, pdsch_mcs_table mcs_table)
{
  cqi_table_t cqi_table;
  // Set CQI table according to the MCS table used for PDSCH.
  switch (mcs_table) {
    case pdsch_mcs_table::qam64:
      cqi_table = cqi_table_t::table1;
      break;
    case pdsch_mcs_table::qam256:
      cqi_table = cqi_table_t::table2;
      break;
    case pdsch_mcs_table::qam64LowSe:
      cqi_table = cqi_table_t::table3;
      break;
    default:
      report_error("Invalid MCS table={}\n", mcs_table);
  }

  // Set MCS index table for PDSCH. See TS 38.214, Table 5.1.3.1-[1-3].
  if (cell_cfg.init_dl_bwp.pdsch_cfg.has_value()) {
    cell_cfg.init_dl_bwp.pdsch_cfg->mcs_table = mcs_table;
  }

  if (cell_cfg.csi_meas_cfg.has_value()) {
    for (auto& csi_report_cfg : cell_cfg.csi_meas_cfg.value().csi_report_cfg_list) {
      // Set CQI table. See TS 38.214, Table 5.2.2.1-[1-4].
      csi_report_cfg.cqi_table = cqi_table;
    }
  }
}

// Configure dedicated UE configuration to set PUSCH MCS.
static void set_pusch_mcs_table(serving_cell_config& cell_cfg, pusch_mcs_table mcs_table)
{
  // Set MCS index table for PDSCH. See TS 38.214, Table 5.1.3.1-[1-3].
  if (cell_cfg.ul_config.has_value() and cell_cfg.ul_config->init_ul_bwp.pusch_cfg.has_value()) {
    cell_cfg.ul_config->init_ul_bwp.pusch_cfg->mcs_table = mcs_table;
  }
}

// Configure dedicated UE configuration to set UL-MIMO related parameters.
static void set_ul_mimo(serving_cell_config&      cell_cfg,
                        unsigned                  max_rank,
                        unsigned                  nof_srs_ports,
                        tx_scheme_codebook_subset codebook_subset)
{
  // Skip if the UL configuration is not present.
  if (SRSRAN_UNLIKELY(!cell_cfg.ul_config.has_value() || !cell_cfg.ul_config->init_ul_bwp.pusch_cfg ||
                      !cell_cfg.ul_config->init_ul_bwp.srs_cfg)) {
    return;
  }

  // Prepare codebook transmission parameters.
  cell_cfg.ul_config->init_ul_bwp.pusch_cfg->tx_cfg =
      tx_scheme_codebook{.max_rank = max_rank, .codebook_subset = codebook_subset};

  // Force the number of ports for all SRS resources to the maximum the UE supports.
  for (auto& srs_res : cell_cfg.ul_config->init_ul_bwp.srs_cfg->srs_res_list) {
    srs_res.nof_ports = static_cast<srs_config::srs_resource::nof_srs_ports>(nof_srs_ports);
  }
}

ue_capability_manager::ue_capability_manager(span<const du_cell_config> cell_cfg_list_, srslog::basic_logger& logger_) :
  base_cell_cfg_list(cell_cfg_list_), logger(logger_)
{
}

void ue_capability_manager::update(du_ue_resource_config& ue_res_cfg, const byte_buffer& ue_cap_rat_list)
{
  // Decode new UE capabilities.
  if (not decode_ue_capability_list(ue_cap_rat_list) and not first_update) {
    // No changes detected in the UE capabilities, and update(...) was called before. In this case, we can do not need
    // to apply any extra changes to the ue_res_cfg that weren't already applied.
    return;
  }
  first_update = false;

  du_cell_index_t      cell_idx  = to_du_cell_index(0);
  serving_cell_config& pcell_cfg = ue_res_cfg.cell_group.cells[cell_idx].serv_cell_cfg;

  // Enable 256QAM for PDSCH, if supported.
  set_pdsch_mcs_table(pcell_cfg, select_pdsch_mcs_table(cell_idx));

  // Enable 256QAM for PUSCH, if supported.
  set_pusch_mcs_table(pcell_cfg, select_pusch_mcs_table(cell_idx));

  // Setup UL MIMO parameters.
  set_ul_mimo(
      pcell_cfg, select_pusch_max_rank(cell_idx), select_srs_nof_ports(cell_idx), select_tx_codebook_subset(cell_idx));
}

bool ue_capability_manager::decode_ue_capability_list(const byte_buffer& ue_cap_rat_list)
{
  using namespace asn1::rrc_nr;

  if (ue_cap_rat_list.empty()) {
    // No update.
    return false;
  }

  ue_cap_rat_container_list_l asn1_cap_list;
  {
    asn1::cbit_ref bref{ue_cap_rat_list};
    if (asn1::unpack_dyn_seq_of(asn1_cap_list, bref, 0, 8) != asn1::SRSASN_SUCCESS) {
      logger.error("Couldn't unpack UE Capability RAT Container List RRC container");
      return false;
    }
  }

  for (const ue_cap_rat_container_s& ue_cap_rat : asn1_cap_list) {
    if (ue_cap_rat.rat_type.value != rat_type_opts::nr) {
      logger.warning("Unsupported RAT type in UE Capability RAT Container List RRC container");
      continue;
    }
    expected<ue_capability_summary, std::string> asn1_cap = decode_ue_nr_cap_container(ue_cap_rat.ue_cap_rat_container);
    if (not asn1_cap.has_value()) {
      logger.warning("{}", asn1_cap.error());
      continue;
    }
    ue_caps = asn1_cap.value();
    return true;
  }

  return false;
}

pdsch_mcs_table ue_capability_manager::select_pdsch_mcs_table(du_cell_index_t cell_idx) const
{
  const auto& init_dl_bwp = base_cell_cfg_list[cell_idx].ue_ded_serv_cell_cfg.init_dl_bwp;

  if (not init_dl_bwp.pdsch_cfg.has_value()) {
    // No base cell PDSCH config. Default to QAM64.
    return pdsch_mcs_table::qam64;
  }
  // TODO: Support dynamic change of the DL MCS table based on the UE capabilities. This requires changes in the
  //  scheduler.
  return init_dl_bwp.pdsch_cfg->mcs_table;
}

pusch_mcs_table ue_capability_manager::select_pusch_mcs_table(du_cell_index_t cell_idx) const
{
  nr_band     band        = base_cell_cfg_list[cell_idx].ul_carrier.band;
  const auto& base_ul_cfg = base_cell_cfg_list[cell_idx].ue_ded_serv_cell_cfg.ul_config;

  if (not base_ul_cfg.has_value() or not base_ul_cfg->init_ul_bwp.pusch_cfg.has_value() or not ue_caps.has_value()) {
    // No PUSCH config or no UE capabilities decoded yet. Default to QAM64.
    return pusch_mcs_table::qam64;
  }

  if (base_ul_cfg->init_ul_bwp.pusch_cfg->mcs_table == pusch_mcs_table::qam256) {
    // If the band capability is present, select the MCS table from this band.
    if (ue_caps->bands.count(band)) {
      return ue_caps->bands.at(band).pusch_qam256_supported ? pusch_mcs_table::qam256 : pusch_mcs_table::qam64;
    }

    // In case the preferred MCS table is 256QAM, but the UE does not support it, we default to QAM64.
    if (std::none_of(ue_caps->bands.begin(), ue_caps->bands.end(), [](const auto& b) {
          return b.second.pusch_qam256_supported;
        })) {
      return pusch_mcs_table::qam64;
    }
  }
  return base_ul_cfg->init_ul_bwp.pusch_cfg->mcs_table;
}

tx_scheme_codebook_subset ue_capability_manager::select_tx_codebook_subset(du_cell_index_t cell_idx) const
{
  nr_band band = base_cell_cfg_list[cell_idx].ul_carrier.band;

  // If UE capabilities or the band are not available, return default value.
  if (not ue_caps.has_value() || ue_caps->bands.count(band) == 0) {
    return ue_capability_summary::default_pusch_tx_coherence;
  }

  return ue_caps->bands.at(band).pusch_tx_coherence;
}

unsigned ue_capability_manager::select_srs_nof_ports(du_cell_index_t cell_idx) const
{
  nr_band band = base_cell_cfg_list[cell_idx].ul_carrier.band;

  // If UE capabilities or the band are not available, return default value.
  if (not ue_caps.has_value() || ue_caps->bands.count(band) == 0) {
    return ue_capability_summary::default_nof_srs_tx_ports;
  }

  return ue_caps->bands.at(band).nof_srs_tx_ports;
}

unsigned ue_capability_manager::select_pusch_max_rank(du_cell_index_t cell_idx) const
{
  nr_band band = base_cell_cfg_list[cell_idx].ul_carrier.band;

  // Configured maximum number of layers.
  unsigned pusch_max_rank = base_cell_cfg_list[cell_idx].pusch_max_nof_layers;

  // If UE capabilities or the band are not available, return default value.
  if (not ue_caps.has_value() || ue_caps->bands.count(band) == 0) {
    return std::min(pusch_max_rank, ue_capability_summary::default_pusch_max_rank);
  }

  return std::min(pusch_max_rank, ue_caps->bands.at(band).pusch_max_rank);
}
