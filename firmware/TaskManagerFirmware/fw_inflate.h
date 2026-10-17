#pragma once
// =====================================================================
// fw_inflate.h —— 自带 DEFLATE/gzip 解压（2026-10-02）
// 为什么需要它：
//   本项目用的 esp32:esp32 3.3.12-cn 工具链**不含 zlib**
//   （全盘搜索无 zlib.h / libz.a），故 __has_include(<zlib.h>) 为假。
//   而和风天气**无视 Accept-Encoding: identity，仍强制返回 GZIP**
//   （2026-10-02 实测），于是天气拉取必然以 E_NET_GZIP 失败。
//   GitHub 直连/镜像均被限流（404），无法安装官方 Zlib 库，
//   故自带一个精简 inflate（RFC 1951 DEFLATE + RFC 1952 gzip 封装）。
//
// 正确性验证：Python 原型（tools_inflate_proto.py）对照 zlib 解压，
//   覆盖 stored / fixed-Huffman / dynamic-Huffman 三种块类型 + 空数据，
//   真实 QWeather now/24h 响应亦通过，8/8 且 CRC32 全部校验一致。
//
// 边界：仅支持 gzip 封装（0x1F 0x8B）。raw deflate 需调用 inflateRaw()。
// 内存：输出缓冲由调用方提供（String），本模块不额外分配整块副本。
// =====================================================================
#include <Arduino.h>

class Inflate {
public:
  // 解压 gzip（自动跳过 gzip 头、校验 CRC32/ISIZE）
  // 成功返回 true 并填充 out；失败返回 false（失败原因见 lastError()）
  static bool gunzip(const uint8_t* src, size_t srclen, String& out);

  // 解压 raw deflate（无 gzip 头）
  static bool inflateRaw(const uint8_t* src, size_t srclen, String& out);

  // 最近一次失败的原因（仅用于串口日志，不影响功能）
  static const char* lastError();

  // 预设输出上限（字节）；0 = 不限。防止损坏数据把 RAM 撑爆。
  static void setOutputLimit(uint32_t bytes) { _limit = bytes; }

private:
  // —— 位读取器（LSB 先行，符合 DEFLATE 规范）——
  const uint8_t* _src = nullptr;
  size_t  _srclen = 0;
  size_t  _pos = 0;        // 字节位置
  uint8_t _bit = 0;        // 当前字节内已读的位数
  uint32_t _bitCnt = 0;    // 已读位数（防越界计数溢出）
  bool     _bad = false;   // 越界/格式错误标志
  const char* _err = "";

  // —— 输出 ——
  String* _out = nullptr;
  uint32_t _outLen = 0;
  uint32_t _outCap = 0;    // 0 = 不限

  // —— Huffman 表（RFC 1951 规范解码法：用 counts + symbols 逐位累积）——
  // 码长最多 15 位（literal）或 15 位（distance），故表长 16。
  uint16_t _litCounts[16], _litSymbols[288];
  uint16_t _distCounts[16], _distSymbols[30];
  uint16_t _clCounts[16],  _clSymbols[19];

  uint32_t _bitRead(int n);
  void     alignByte() { if (_bit) { _bit = 0; _pos++; } }
  bool     buildHuff(const uint8_t* lengths, int n,
                     uint16_t* counts, uint16_t* symbols);
  int      decodeSym(const uint16_t* counts, const uint16_t* symbols, int maxLen);
  bool     block(const uint16_t* lc, const uint16_t* ls,
                 const uint16_t* dc, const uint16_t* ds);
  void     put(uint8_t b);
  void     putCopy(uint32_t dist, uint32_t len);
  bool     inflateCore(bool isGzip, String& out);
  void     fail(const char* why) {
    if (!_bad) { _bad = true; _err = why; Inflate::_lastErr = why; }
  }

  static const char* _lastErr;   // 最近失败原因（供日志）
  static uint32_t    _limit;    // 输出上限，0 = 不限
};