#pragma once
// =====================================================================
// fw_time.h —— 时间服务（总纲第 6 节）
// SNTP 对时（ntp.aliyun.com 备 pool.ntp.org，GMT+8）、毫秒时间戳
// 一切时间戳取自本服务
// =====================================================================
#include <Arduino.h>

class TimeService {
public:
  void begin();                 // 配置 SNTP（异步对时，不阻塞首屏）
  bool synced();                // SNTP 是否已对上（年份 > 2020）
  uint32_t epoch();             // Unix 秒（未同步返回 0）
  static uint32_t millisMs();   // 毫秒时间戳（开机起算）
  String hhmm();                // "HH:MM"，未同步返回 "--:--"
  String dateStr();             // "YYYY-MM-DD"，未同步返回 "----.--.--"
  String csvDateTime();         // "YYYY-MM-DD HH:MM"（历史记录用），未同步返回 "--"
  String hhmmOf(uint32_t ts);   // 指定时间戳的 "HH:MM"（数据年龄标注用）
  String ageStr(uint32_t ts);   // 数据年龄描述："3 分钟前" / "2 小时前" / "5 天前"
  String ageShort(uint32_t ts); // 短年龄："3m" / "2h" / "5d"，未同步 "--"
};

extern TimeService Time;
