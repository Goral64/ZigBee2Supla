// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <log.h>  // supla-common: supla_log(), LOG_* levels

#include <string>

#define Z2S_LOG_ERROR(...) supla_log(LOG_ERR, __VA_ARGS__)
#define Z2S_LOG_WARNING(...) supla_log(LOG_WARNING, __VA_ARGS__)
#define Z2S_LOG_INFO(...) supla_log(LOG_INFO, __VA_ARGS__)
#define Z2S_LOG_DEBUG(...) supla_log(LOG_DEBUG, __VA_ARGS__)

namespace z2s {

// Accepts: error, warning, info, debug, verbose. Returns false for unknown.
bool setLogLevel(const std::string &level);

}  // namespace z2s
