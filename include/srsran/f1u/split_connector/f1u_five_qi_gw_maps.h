/*
 *
 *
 * Copyright 2021-2024 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "srsran/gtpu/gtpu_gateway.h"
#include "srsran/ran/qos/five_qi.h"
#include <map>

namespace srsran::srs_cu_up {

struct gtpu_gateway_maps {
  std::vector<std::unique_ptr<gtpu_gateway>>                      default_gws;
  std::map<five_qi_t, std::vector<std::unique_ptr<gtpu_gateway>>> five_qi_gws;
};

} // namespace srsran::srs_cu_up
