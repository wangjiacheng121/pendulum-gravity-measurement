// =====================================================================
// fw_time.cpp —— 时间服务实现
// =====================================================================
#include "fw_time.h"
#include <time.h>
#include <sys/time.h>

TimeService Time;

void TimeService::begin() {
  // GMT+8，无夏令时；SNTP 异步运行，不阻塞首屏（总纲 5.5 上电时序）
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
}

bool TimeService::synced() {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  return (tmv.tm_year + 1900) > 2020;
}

uint32_t TimeService::epoch() {
  if (!synced()) return 0;
  return (uint32_t)time(nullptr);
}

uint32_t TimeService::millisMs() {
  return (uint32_t)millis();
}

String TimeService::hhmm() {
  if (!synced()) return "--:--";
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char buf[16];   // 原 8 贴近阈值，扩容防御性加固（tm_* 为 int，理论最宽输出 >8）
  snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return String(buf);
}

String TimeService::dateStr() {
  if (!synced()) return "----.--.--";
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char buf[32];   // 编译器无法推断 tm_* 的取值范围，缓冲留足避免 -Wformat-truncation
  snprintf(buf, sizeof(buf), "%04d.%02d.%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  return String(buf);
}

String TimeService::csvDateTime() {
  if (!synced()) return "--";
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char buf[64];   // 理论最坏情况 60 字节（GCC 按 tm_* 为任意 int 估算），留足以消除 -Wformat-truncation
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
  return String(buf);
}

String TimeService::hhmmOf(uint32_t ts) {
  if (ts == 0 || !synced()) return "--:--";
  time_t t = (time_t)ts;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[16];   // 同 hhmm()
  snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return String(buf);
}

String TimeService::ageShort(uint32_t ts) {
  if (ts == 0) return "--";
  uint32_t now = epoch();
  if (now == 0) return "--";
  uint32_t age = (now > ts) ? (now - ts) : 0;
  char buf[16];
  if (age < 3600)       snprintf(buf, sizeof(buf), "%lum",  (unsigned long)(age / 60));
  else if (age < 86400) snprintf(buf, sizeof(buf), "%luh",  (unsigned long)(age / 3600));
  else                  snprintf(buf, sizeof(buf), "%lud",  (unsigned long)(age / 86400));
  return String(buf);
}

String TimeService::ageStr(uint32_t ts) {
  if (ts == 0) return "无缓存";
  uint32_t now = epoch();
  if (now == 0) return "时间未同步";
  uint32_t age = (now > ts) ? (now - ts) : 0;
  char buf[32];
  if (age < 60)       snprintf(buf, sizeof(buf), "%lus前",  (unsigned long)age);
  else if (age < 3600) snprintf(buf, sizeof(buf), "%lu分钟前", (unsigned long)(age / 60));
  else if (age < 86400) snprintf(buf, sizeof(buf), "%lu小时前", (unsigned long)(age / 3600));
  else                  snprintf(buf, sizeof(buf), "%lu天前",   (unsigned long)(age / 86400));
  return String(buf);
}
