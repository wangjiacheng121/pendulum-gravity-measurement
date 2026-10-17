#pragma once
// =====================================================================
// fw_display.h —— 显示服务（SH1106 128x64 OLED，U8g2 全缓冲）
// 契约（总纲第 6 节）：
//  - 屏幕独占授权制：仅活跃程序持有；挂起即被强制回收
//  - 统一缓冲后整帧提交（clearBuffer → 绘制 → commit）
//  - 通知条能力：叠加显示 3-5s 自动消失（程序1 表情联动出口，不抢屏）
//  - L3 熄屏：sleep() = SH1106 关显示指令；wake() 恢复
// =====================================================================
#include <Arduino.h>
#include <U8g2lib.h>
#include "fw_types.h"

class DisplayService {
public:
  void begin();                       // Wire.begin(8,9) 之后调用；探测 0x3C/0x3D 并初始化
  bool present() const { return _ok; }            // OLED 是否初始化成功
  uint8_t address() const { return _addr; }

  // ---- 授权（由 AppManager 调用）----
  void grant(AppId who);
  void revoke();
  AppId holder() const { return _holder; }

  // ---- 绘制原语（仅活跃程序调用；整帧三步走）----
  void clearBuffer()                        { if (_ok) _u8g2.clearBuffer(); }
  void setFont(const uint8_t* f)            { if (_ok) _u8g2.setFont(f); }
  void drawStr(int x, int y, const String& s){ if (_ok) _u8g2.drawStr(x, y, s.c_str()); }
  void drawUtf8(int x, int y, const String& s){ if (_ok) _u8g2.drawUTF8(x, y, s.c_str()); }
  void drawBox(int x, int y, int w, int h)  { if (_ok) _u8g2.drawBox(x, y, w, h); }
  void drawFrame(int x, int y, int w, int h){ if (_ok) _u8g2.drawFrame(x, y, w, h); }
  void drawRFrame(int x, int y, int w, int h, int r) { if (_ok) _u8g2.drawRFrame(x, y, w, h, r); }
  void drawDisc(int x, int y, int r)        { if (_ok) _u8g2.drawDisc(x, y, r); }
  void drawCircle(int x, int y, int r)      { if (_ok) _u8g2.drawCircle(x, y, r); }
  void drawLine(int x0, int y0, int x1, int y1) { if (_ok) _u8g2.drawLine(x0, y0, x1, y1); }
  int  getStrWidth(const String& s)         { return _ok ? _u8g2.getStrWidth(s.c_str()) : 0; }
  int  getUtf8Width(const String& s)        { return _ok ? _u8g2.getUTF8Width(s.c_str()) : 0; }
  void rightUtf8(int y, const String& s);   // 右对齐 UTF8 文本
  void rightStr(int y, const String& s);    // 右对齐 ASCII 文本
  void centerStr(int y, const String& s);   // 水平居中绘制
  void centerUtf8(int y, const String& s);  // 水平居中 UTF8 文本

  // ---- 整帧提交：自动叠加通知条（若在显示期内）----
  void commit();

  // ---- 通知条（程序1 表情联动浮现出口；3-5s 自动消失）----
  void showNotification(const String& text, uint32_t durMs = 4000);

  // ---- L3 熄屏协作 ----
  void sleep();                       // SH1106 关显示
  void wake();                        // 恢复显示
  bool asleep() const { return _asleep; }

  // 常用字体（集中定义，避免各模块重复引用）
  static const uint8_t* FONT_BIG;     // 10x20 ASCII，颜文字用
  static const uint8_t* FONT_MID;     // 8x16 符号
  static const uint8_t* FONT_SMALL;   // 6x12 ASCII
  static const uint8_t* FONT_ROW;     // 6x10 ASCII（程序2 四行布局）
  static const uint8_t* FONT_TINY;    // 5x8 ASCII 角标
  static const uint8_t* FONT_NUM;     // logisoso28 数字大字（气温/风速）
  static const uint8_t* FONT_CN12;    // 文泉驿 12pt 中文（GB2312，UTF-8 输入）
  static const uint8_t* FONT_CN16;    // 文泉驿 16pt 中文（GB2312，UTF-8 输入）

private:
  U8G2_SH1106_128X64_NONAME_F_HW_I2C _u8g2{U8G2_R0, U8X8_PIN_NONE};
  bool      _ok = false;
  bool      _asleep = false;
  uint8_t   _addr = 0;
  AppId     _holder = AppId::PET;
  String    _notifText;
  uint32_t  _notifUntil = 0;
};

extern DisplayService Display;
