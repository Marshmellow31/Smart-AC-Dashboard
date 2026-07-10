#include "TimeManager.h"

#include <time.h>

#include "EventLog.h"

void TimeManager::onWifiConnected() {
  if (configured_) return;
  configured_ = true;
  // POSIX TZ: IST is UTC+5:30, no DST.
  configTzTime("IST-5:30", "pool.ntp.org", "time.google.com", "time.nist.gov");
  Serial.println("NTP sync started (IST).");
}

void TimeManager::loop() {
  if (!syncLogged_ && isTimeValid()) {
    syncLogged_ = true;
    log_.add(CmdSource::SYSTEM, "NTP time synced: %s", format(now()).c_str());
  }
}

String TimeManager::format(time_t t) const {
  struct tm tmInfo;
  localtime_r(&t, &tmInfo);
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmInfo);
  return String(buf);
}
