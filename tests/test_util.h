// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

inline nlohmann::json loadTestJson(const std::string &name) {
  std::ifstream in(std::string(Z2S_TEST_DATA_DIR) + "/" + name);
  nlohmann::json j;
  in >> j;
  return j;
}
