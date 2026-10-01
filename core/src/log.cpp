// SPDX-License-Identifier: GPL-2.0-or-later
// Implementation of the supla-common logging API (log.h) for z2s.

#include "z2s/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include <mutex>

namespace {
int gLogLevel = LOG_INFO;
std::mutex gLogMutex;

const char *levelName(int pri) {
  switch (pri) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
    case LOG_ERR:
      return "ERR";
    case LOG_WARNING:
      return "WRN";
    case LOG_NOTICE:
    case LOG_INFO:
      return "INF";
    case LOG_DEBUG:
      return "DBG";
    default:
      return "VRB";
  }
}
}  // namespace

extern "C" {

void supla_log_set_level(int level) { gLogLevel = level; }

int supla_log_get_level(void) { return gLogLevel; }

char supla_log_is_enabled(int level) { return level <= gLogLevel ? 1 : 0; }

void supla_log(int pri, const char *fmt, ...) {
  if (pri > gLogLevel || fmt == nullptr) {
    return;
  }
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tms;
  time_t t = tv.tv_sec;
  localtime_r(&t, &tms);

  std::lock_guard<std::mutex> lock(gLogMutex);
  fprintf(stderr, "%s [%02d:%02d:%02d.%03d] %s\n", levelName(pri), tms.tm_hour,
          tms.tm_min, tms.tm_sec, static_cast<int>(tv.tv_usec / 1000), buf);
}

void supla_write_state_file(const char *, int, const char *, ...) {}

}  // extern "C"

namespace z2s {

bool setLogLevel(const std::string &level) {
  if (level == "error") {
    supla_log_set_level(LOG_ERR);
  } else if (level == "warning") {
    supla_log_set_level(LOG_WARNING);
  } else if (level == "info") {
    supla_log_set_level(LOG_INFO);
  } else if (level == "debug") {
    supla_log_set_level(LOG_DEBUG);
  } else if (level == "verbose") {
    supla_log_set_level(LOG_VERBOSE);
  } else {
    return false;
  }
  return true;
}

}  // namespace z2s
