/*
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "apps/units/flexible_du/flexible_du_application_unit.h"
#include "dynamic_du_unit_config.h"

namespace srsran {

/// \brief Dynamic DU application unit implementation.
///
/// A DU of type Dynamic adds support to all the types of Radio Units.
class dynamic_du_application_unit_impl : public flexible_du_application_unit
{
public:
  explicit dynamic_du_application_unit_impl(std::string_view app_name);

  // See interface for documentation.
  void on_parsing_configuration_registration(CLI::App& app) override;

  // See interface for documentation.
  void on_configuration_parameters_autoderivation(CLI::App& app) override;

  // See interface for documentation.
  bool on_configuration_validation(const os_sched_affinity_bitmask& available_cpus) const override;

  // See interface for documentation.
  void on_loggers_registration() override;

  // See interface for documentation.
  du_unit create_flexible_du_unit(const du_unit_dependencies& dependencies) override;

  // See interface for documentation.
  du_high_unit_config&       get_du_high_unit_config() override { return unit_cfg.du_high_cfg.config; }
  const du_high_unit_config& get_du_high_unit_config() const override { return unit_cfg.du_high_cfg.config; }

  // See interface for documentation.
  void dump_config(YAML::Node& node) const override;

  // See interface for documentation.
  void fill_worker_manager_config(worker_manager_config& config) override;

private:
  dynamic_du_unit_config unit_cfg;
};

} // namespace srsran
