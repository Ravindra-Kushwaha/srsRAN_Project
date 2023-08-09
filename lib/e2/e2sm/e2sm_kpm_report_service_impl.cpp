/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "e2sm_kpm_report_service_impl.h"

using namespace asn1::e2ap;
using namespace asn1::e2sm_kpm;
using namespace srsran;

e2sm_kpm_report_service_base::e2sm_kpm_report_service_base(asn1::e2sm_kpm::e2_sm_kpm_action_definition_s action_def_,
                                                           e2_du_metrics_interface& du_metrics_interface_) :
  logger(srslog::fetch_basic_logger("E2SM-KPM")),
  action_def_generic(action_def_),
  du_metrics_interface(du_metrics_interface_)
{
  // initialize RIC indication header
  ric_ind_header_generic.ind_hdr_formats.ind_hdr_format1().vendor_name_present        = false;
  ric_ind_header_generic.ind_hdr_formats.ind_hdr_format1().sender_name_present        = false;
  ric_ind_header_generic.ind_hdr_formats.ind_hdr_format1().sender_type_present        = false;
  ric_ind_header_generic.ind_hdr_formats.ind_hdr_format1().file_formatversion_present = false;
  ric_ind_header_generic.ind_hdr_formats.ind_hdr_format1().collet_start_time.from_number(0);
}

bool e2sm_kpm_report_service_base::check_measurement_name(meas_type_c meas_type, const char* meas)
{
  if (strcmp(meas_type.meas_name().to_string().c_str(), meas) == 0) {
    return true;
  }
  return false;
}

srsran::byte_buffer e2sm_kpm_report_service_base::get_indication_message()
{
  byte_buffer   ind_msg_bytes;
  asn1::bit_ref bref_ind_msg(ind_msg_bytes);
  if (ric_ind_message_generic.pack(bref_ind_msg) != asn1::SRSASN_SUCCESS) {
    logger.error("Failed to pack RIC Indication Message");
  };
  return ind_msg_bytes;
}

srsran::byte_buffer e2sm_kpm_report_service_base::get_indication_header()
{
  byte_buffer   ind_hdr_bytes;
  asn1::bit_ref bref_ind_hdr(ind_hdr_bytes);
  if (ric_ind_header_generic.pack(bref_ind_hdr) != asn1::SRSASN_SUCCESS) {
    logger.error("Failed to pack RIC Indication header");
  };

  return ind_hdr_bytes;
}

e2sm_kpm_report_service_style1::e2sm_kpm_report_service_style1(e2_sm_kpm_action_definition_s action_def_,
                                                               e2_du_metrics_interface&      du_metrics_interface_) :
  e2sm_kpm_report_service_base(action_def_, du_metrics_interface_),
  action_def(action_def_generic.action_definition_formats.action_definition_format1()),
  ric_ind_message(ric_ind_message_generic.ind_msg_formats.set_ind_msg_format1())
{
  granul_period = action_def.granul_period;
  if (cell_global_id_present) {
    cell_global_id = action_def.cell_global_id;
  }
}

bool e2sm_kpm_report_service_style1::collect_measurements()
{
  // clear collected data
  for (uint32_t i = 0; i < ric_ind_message.meas_data.size(); ++i) {
    ric_ind_message.meas_data[i].meas_record.clear();
  }
  ric_ind_message.meas_data.clear();
  ric_ind_message.meas_info_list.clear();

  // Set the granularity period (TODO: disable as currently not supported in flexric)
  ric_ind_message.granul_period_present = false;
  // ric_ind_message.granul_period         = action_def.granul_period;

  scheduler_ue_metrics ue_metrics = {};
  du_metrics_interface.get_metrics(ue_metrics);
  // Fill indication msg
  auto& meas_info_list = action_def.meas_info_list;
  for (auto& meas_info : meas_info_list) {
    if (check_measurement_name(meas_info.meas_type, "RSRP")) {
      meas_info_item_s meas_info_item;
      meas_info_item.meas_type = meas_info.meas_type;
      label_info_item_s label_info_item;
      label_info_item.meas_label.no_label_present = true;
      label_info_item.meas_label.no_label         = meas_label_s::no_label_e_::true_value;
      meas_info_item.label_info_list.push_back(label_info_item);
      ric_ind_message.meas_info_list.push_back(meas_info_item);
      meas_data_item_s   meas_data_item;
      meas_record_item_c meas_record_item;
      meas_record_item.set_integer() = (int)ue_metrics.pusch_snr_db;
      meas_data_item.meas_record.push_back(meas_record_item);
      ric_ind_message.meas_data.push_back(meas_data_item);
    }
  }

  return true;
}

e2sm_kpm_report_service_style3::e2sm_kpm_report_service_style3(e2_sm_kpm_action_definition_s action_def_,
                                                               e2_du_metrics_interface&      du_metrics_interface_) :
  e2sm_kpm_report_service_base(action_def_, du_metrics_interface_),
  action_def(action_def_generic.action_definition_formats.action_definition_format3()),
  ric_ind_message(ric_ind_message_generic.ind_msg_formats.set_ind_msg_format2())
{
  granul_period = action_def.granul_period;
  if (cell_global_id_present) {
    cell_global_id = action_def.cell_global_id;
  }
}

void e2sm_kpm_report_service_style3::add_matching_condition_item(const char*            name,
                                                                 meas_cond_ueid_item_s& cond_ueid_item,
                                                                 matching_cond_item_s&  match_cond_item)
{
  cond_ueid_item.meas_type.set_meas_name().from_string(name);
  cond_ueid_item.matching_cond.push_back(match_cond_item);
}

bool e2sm_kpm_report_service_style3::collect_measurements()
{
  // clear collected data
  ric_ind_message.meas_data.clear();
  ric_ind_message.meas_cond_ueid_list.clear();

  // Set the granularity period
  ric_ind_message.granul_period_present = true;
  ric_ind_message.granul_period         = action_def.granul_period;

  // Get UE metrics from the DU metrics interface
  scheduler_ue_metrics ue_metrics = {};
  du_metrics_interface.get_metrics(ue_metrics);

  // Resize the measurement data and measurement condition UE ID lists to the number of UEs
  size_t num_ues = 1; // TODO get number of UEs from DU
  ric_ind_message.meas_data.resize(num_ues);
  ric_ind_message.meas_cond_ueid_list.resize(num_ues);

  // Loop over each UE and each measurement condition in the action definition
  for (unsigned int j = 0; j < num_ues; j++) {
    auto& meas_cond_list = action_def.meas_cond_list;
    for (auto& meas_cond : meas_cond_list) {
      // Check the measurement name and add a matching condition item to the UE's measurement condition UE ID list
      if (check_measurement_name(meas_cond.meas_type, "CQI")) {
        matching_cond_item_s match_cond_item;
        match_cond_item.matching_cond_choice.set_test_cond_info();
        match_cond_item.matching_cond_choice.test_cond_info().test_type.set_cqi();
        match_cond_item.matching_cond_choice.test_cond_info().test_value.set_value_int();
        match_cond_item.matching_cond_choice.test_cond_info().test_value.value_int() = ue_metrics.cqi;
        add_matching_condition_item("CQI", ric_ind_message.meas_cond_ueid_list[j], match_cond_item);
      } else if (check_measurement_name(meas_cond.meas_type, "RSRP")) {
        matching_cond_item_s match_cond_item;
        match_cond_item.matching_cond_choice.set_test_cond_info();
        match_cond_item.matching_cond_choice.test_cond_info().test_type.set_rsrp();
        match_cond_item.matching_cond_choice.test_cond_info().test_value.set_value_int();
        match_cond_item.matching_cond_choice.test_cond_info().test_value.value_int() = ue_metrics.pusch_snr_db;
        add_matching_condition_item("RSRP", ric_ind_message.meas_cond_ueid_list[j], match_cond_item);
      } else if (check_measurement_name(meas_cond.meas_type, "RSRQ")) {
        matching_cond_item_s match_cond_item;
        match_cond_item.matching_cond_choice.set_test_cond_info();
        match_cond_item.matching_cond_choice.test_cond_info().test_type.set_rsrq();
        match_cond_item.matching_cond_choice.test_cond_info().test_value.set_value_int();
        match_cond_item.matching_cond_choice.test_cond_info().test_value.value_int() = ue_metrics.pusch_snr_db;
        add_matching_condition_item("RSRQ", ric_ind_message.meas_cond_ueid_list[j], match_cond_item);
      } else {
        logger.error("Unknown meas type %s", meas_cond.meas_type.meas_name().to_string().c_str());
      }
    }
  }
  return true;
}