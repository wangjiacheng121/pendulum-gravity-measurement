// =====================================================================
// fw_display.cpp —— 显示服务实现
// =====================================================================
#include "fw_display.h"
#include "fw_config.h"
#include <Wire.h>

DisplayService Display;

const uint8_t* DisplayService::FONT_BIG   = u8g2_font_10x20_tf;   // ASCII，颜文字用
const uint8_t* DisplayService::FONT_MID   = u8g2_font_unifont_t_symbols; // 含符号
const uint8_t* DisplayService::FONT_SMALL = u8g2_font_6x12_tf;
const uint8_t* DisplayService::FONT_ROW   = u8g2_font_6x10_tf;    // 程序2 四行布局
const uint8_t* DisplayService::FONT_TINY  = u8g2_font_5x8_tf;     // 角标
const uint8_t* DisplayService::FONT_NUM   = u8g2_font_logisoso28_tn; // 数字大字
const uint8_t* DisplayService::FONT_CN12  = u8g2_font_wqy12_t_gb2312; // 中文（UTF-8 输入）
const uint8_t* DisplayService::FONT_CN16  = u8g2_font_wqy16_t_gb2312;

void DisplayService::begin() {
  _ok = false;
  _asleep = false;
  _holder = AppId::PET;
  _notifText = "";
  _notifUntil = 0;

  // SH1106 专用驱动（1.3" 屏与 SSD1306 指令集不完全兼容，禁止换 SSD1306 库）
  // ESP32-S3 需先 Wire.begin(8,9) 指定 I2C 引脚（总纲第 3 节）
  _u8g2.setBusClock(400000);

  // I2C 探测：首选 0x3C，失败试 0x3D
  _addr = 0;
  Wire.beginTransmission(OLED_ADDR_1);
  if (Wire.endTransmission() == 0) _addr = OLED_ADDR_1;
  else {
    Wire.beginTransmission(OLED_ADDR_2);
    if (Wire.endTransmission() == 0) _addr = OLED_ADDR_2;
  }
  if (_addr == 0) {
    Serial.println("[Display] ERR: OLED not found on 0x3C/0x3D, check wiring");
    return;   // 初始化失败：程序1 待机页显示错误（这里仅置位，由上层处理）
  }

  _u8g2.setI2CAddress(_addr << 1);   // U8g2 使用 8 位地址
  _u8g2.begin();
  _u8g2.setPowerSave(0);
  _u8g2.clearBuffer();
  _u8g2.setFont(u8g2_font_10x20_tf);
  _u8g2.drawStr(4, 30, FW_NAME);
  _u8g2.setFont(u8g2_font_6x12_tf);
  _u8g2.drawStr(4, 50, "booting...");
  _u8g2.sendBuffer();
  _ok = true;
  Serial.printf("[Display] OK: SH1106 @ 0x%02X\n", _addr);
}

void DisplayService::grant(AppId who) {
  _holder = who;
  _asleep = false;
  if (_ok) _u8g2.setPowerSave(0);
}

void DisplayService::revoke() {
  // 挂起即被强制回收；通知条保留（跨程序联动出口）
}

void DisplayService::centerStr(int y, const String& s) {
  if (!_ok) return;
  int w = _u8g2.getStrWidth(s.c_str());
  int x = (128 - w) / 2;
  if (x < 0) x = 0;
  _u8g2.drawStr(x, y, s.c_str());
}

void DisplayService::centerUtf8(int y, const String& s) {
  if (!_ok) return;
  int w = _u8g2.getUTF8Width(s.c_str());
  int x = (128 - w) / 2;
  if (x < 0) x = 0;
  _u8g2.drawUTF8(x, y, s.c_str());
}

void DisplayService::rightUtf8(int y, const String& s) {
  if (!_ok) return;
  int w = _u8g2.getUTF8Width(s.c_str());
  int x = 127 - w;
  if (x < 0) x = 0;
  _u8g2.drawUTF8(x, y, s.c_str());
}

void DisplayService::rightStr(int y, const String& s) {
  if (!_ok) return;
  int w = _u8g2.getStrWidth(s.c_str());
  int x = 127 - w;
  if (x < 0) x = 0;
  _u8g2.drawStr(x, y, s.c_str());
}

void DisplayService::showNotification(const String& text, uint32_t durMs) {
  if (durMs < 3000) durMs = 3000;    // 契约：通知条 3–5s
  if (durMs > 5000) durMs = 5000;
  _notifText = text;
  _notifUntil = millis() + durMs;
}

void DisplayService::commit() {
  if (!_ok || _asleep) return;

  // 通知条叠加：底部反白条，不打断当前程序画面（分册01第1节）
  // 环绕安全写法：millis() 约 49.7 天回绕后，裸比较 _notifUntil > millis() 会恒为假
  // （millis() 溢出前的那条通知永久不显示）或恒为真
  if (_notifText.length() > 0 && (int32_t)(millis() - _notifUntil) < 0) {
    _u8g2.setDrawColor(1);
    _u8g2.drawBox(0, 53, 128, 11);
    _u8g2.setFont(u8g2_font_6x12_tf);
    _u8g2.setDrawColor(0);           // 反白文字
    String t = _notifText;
    // 超宽截断必须按 UTF-8 字符边界删：逐字节 remove 会把汉字切成半个字符，
    // 显示为乱码方块。规则：从尾部回跳跳过全部续字节（10xxxxxx），再连同首字节一起删。
    while (_u8g2.getStrWidth(t.c_str()) > 126 && t.length() > 1) {
      size_t n = t.length();
      while (n > 1 && ((uint8_t)t[n - 1] & 0xC0) == 0x80) n--;  // 尾部续字节
      if (n > 1) n--;                                           // 该字符的首字节
      t.remove(n);
    }
    _u8g2.drawStr(1, 62, t.c_str());
    _u8g2.setDrawColor(1);
  }
  _u8g2.sendBuffer();
}

void DisplayService::sleep() {
  if (!_ok || _asleep) return;
  _u8g2.setPowerSave(1);             // SH1106 关显示指令
  _asleep = true;
  Serial.println("[Display] sleep (L3)");
}

void DisplayService::wake() {
  if (!_ok) return;
  _asleep = false;
  _u8g2.setPowerSave(0);
  Serial.println("[Display] wake");
}
