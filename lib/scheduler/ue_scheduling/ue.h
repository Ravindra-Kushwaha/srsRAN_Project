/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "dl_logical_channel_manager.h"
#include "harq_process.h"
#include "srsgnb/adt/stable_id_map.h"
#include "srsgnb/ran/du_types.h"
#include "srsgnb/scheduler/mac_scheduler.h"
#include "ue_configuration.h"
#include "ul_logical_channel_manager.h"

namespace srsgnb {

/// \brief Context respective to a UE serving cell.
class ue_cell
{
public:
  ue_cell(du_ue_index_t                                ue_index,
          rnti_t                                       crnti_val,
          const cell_configuration&                    cell_cfg_common_,
          const serving_cell_ue_configuration_request& ue_serv_cell) :
    ue_index(ue_index),
    cell_index(ue_serv_cell.cell_index),
    harqs(crnti_val, 52, 16, srslog::fetch_basic_logger("MAC")),
    crnti_(crnti_val),
    ue_cfg(cell_cfg_common_, *ue_serv_cell.serv_cell_cfg)
  {
  }

  const du_ue_index_t   ue_index;
  const du_cell_index_t cell_index;

  rnti_t rnti() const { return crnti_; }

  bwp_id_t active_bwp_id() const { return to_bwp_id(0); }
  bool     is_active() const { return true; }

  const ue_cell_configuration& cfg() const { return ue_cfg; }

  harq_entity harqs;

private:
  rnti_t                crnti_;
  ue_cell_configuration ue_cfg;
};

class ue
{
public:
  ue(const cell_configuration& cell_cfg_common_, const sched_ue_creation_request_message& req) :
    ue_index(req.ue_index),
    crnti(req.crnti),
    cell_cfg_common(cell_cfg_common_),
    log_channels_configs(req.lc_config_list),
    sched_request_configs(req.sched_request_config_list)
  {
    for (unsigned i = 0; i != req.cells.size(); ++i) {
      du_cells[i] = std::make_unique<ue_cell>(ue_index, req.crnti, cell_cfg_common, req.cells[i]);
      ue_cells.push_back(du_cells[i].get());
    }
  }
  ue(const ue&)            = delete;
  ue(ue&&)                 = delete;
  ue& operator=(const ue&) = delete;
  ue& operator=(ue&&)      = delete;

  const du_ue_index_t ue_index;
  const rnti_t        crnti;

  void slot_indication(slot_point sl_tx) {}

  ue_cell* find_cell(du_cell_index_t cell_index)
  {
    srsgnb_assert(cell_index < MAX_CELLS, "Invalid cell_index={}", cell_index);
    return du_cells[cell_index].get();
  }
  const ue_cell* find_cell(du_cell_index_t cell_index) const
  {
    srsgnb_assert(cell_index < MAX_CELLS, "Invalid cell_index={}", cell_index);
    return du_cells[cell_index].get();
  }

  /// \brief Fetch UE cell based on UE-specific cell identifier. E.g. PCell corresponds to ue_cell_index==0.
  ue_cell& get_cell(ue_cell_index_t ue_cell_index)
  {
    srsgnb_assert(ue_cell_index < ue_cells.size(), "Invalid cell_index={}", ue_cell_index);
    return *ue_cells[ue_cell_index];
  }
  const ue_cell& get_cell(ue_cell_index_t ue_cell_index) const
  {
    srsgnb_assert(ue_cell_index < ue_cells.size(), "Invalid cell_index={}", ue_cell_index);
    return *ue_cells[ue_cell_index];
  }

  /// \brief Fetch UE PCell.
  ue_cell&       get_pcell() { return *ue_cells[0]; }
  const ue_cell& get_pcell() const { return *ue_cells[0]; }

  /// \brief Number of cells configured for the UE.
  unsigned nof_cells() const { return ue_cells.size(); }

  bool is_ca_enabled() const { return ue_cells.size() > 1; }

  void activate_cells(bounded_bitset<MAX_NOF_DU_CELLS> activ_bitmap) {}

  /// \brief Handle received SR indication.
  void handle_sr_indication(const sr_indication_message& msg) { ul_lc_ch_mgr.handle_sr_indication(msg); }

  /// \brief Once an UL grant is given, the SR status of the UE must be reset.
  void reset_sr_indication() { ul_lc_ch_mgr.reset_sr_indication(); }

  /// \brief Handles received BSR indication by updating UE UL logical channel states.
  void handle_bsr_indication(const ul_bsr_indication_message& msg) { ul_lc_ch_mgr.handle_bsr_indication(msg); }

  /// \brief Handles MAC CE indication.
  void handle_dl_mac_ce_indication(const dl_mac_ce_indication& msg)
  {
    dl_lc_ch_mgr.handle_mac_ce_indication(msg.ce_lcid);
  }

  /// \brief Handles DL Buffer State indication.
  void handle_dl_buffer_state_indication(const dl_buffer_state_indication_message& msg)
  {
    dl_lc_ch_mgr.handle_dl_buffer_status_indication(msg.lcid, msg.bs);
  }

  void handle_reconfiguration_request(const sched_ue_reconfiguration_message& msg);

  /// \brief Computes the number of pending bytes to be allocated for the first time in UL for a given UE.
  unsigned pending_ul_newtx_bytes() const;

  /// UE DL Logical Channel Manager.
  dl_logical_channel_manager dl_lc_ch_mgr;

private:
  static const size_t MAX_CELLS = 4;

  /// Cell configuration. This is common to all UEs within the same cell.
  const cell_configuration& cell_cfg_common;

  /// List of \c mac-LogicalChannelConfig, TS 38.331; \ref sched_ue_creation_request_message.
  std::vector<logical_channel_config> log_channels_configs;
  /// \c schedulingRequestToAddModList, TS 38.331; \ref sched_ue_creation_request_message.
  std::vector<scheduling_request_to_addmod> sched_request_configs;

  /// List of UE cells indexed by \c du_cell_index_t. If an element is null, it means that the DU cell is not
  /// configured to be used by the UE.
  std::array<std::unique_ptr<ue_cell>, MAX_CELLS> du_cells;

  /// List of UE cells indexed by \c ue_cell_index_t. The size of the list is equal to the number of cells aggregated
  /// and configured for the UE. PCell corresponds to ue_cell_index=0. the first SCell corresponds to ue_cell_index=1,
  /// etc.
  static_vector<ue_cell*, MAX_CELLS> ue_cells;

  /// UE UL Logical Channel Manager.
  ul_logical_channel_manager ul_lc_ch_mgr;
};

/// Container that stores all scheduler UEs.
using ue_list = stable_id_map<du_ue_index_t, ue, MAX_NOF_DU_UES>;

} // namespace srsgnb
