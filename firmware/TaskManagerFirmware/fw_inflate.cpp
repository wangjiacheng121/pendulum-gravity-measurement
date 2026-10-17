// =====================================================================
// fw_inflate.cpp —— 自带 DEFLATE/gzip 解压（见 fw_inflate.h 的必要性说明）
// 算法为 RFC 1951 的标准实现：逐位累积的 canonical Huffman 解码，
// 三种块类型全部支持（stored / fixed / dynamic）。
// 参照实现与验证见 tools_inflate_proto.py（8/8 且 CRC32 全对）。
// =====================================================================
#include "fw_inflate.h"

// —— RFC 1951 长度/距离码表 ——
static const uint16_t LEN_BASE[29] = {
  3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEN_EXTRA[29] = {
  0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DIST_BASE[30] = {
  1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
  1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DIST_EXTRA[30] = {
  0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

// 码长传输顺序（RFC 1951 §3.2.7）
static const uint8_t CLEN_ORDER[19] = {
  16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

// ---------- 位读取 ----------
uint32_t Inflate::_bitRead(int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; i++) {
    if (_pos >= _srclen) { fail("输入意外结束（位读取越界）"); return v; }
    uint32_t b = (_src[_pos] >> _bit) & 1U;
    v |= b << i;
    if (++_bit == 8) { _bit = 0; _pos++; }
  }
  return v;
}

// ---------- canonical Huffman 表构建 ----------
bool Inflate::buildHuff(const uint8_t* lengths, int n,
                        uint16_t* counts, uint16_t* symbols) {
  for (int i = 0; i < 16; i++) counts[i] = 0;
  for (int i = 0; i < n; i++) {
    if (lengths[i] > 15) return false;      // DEFLATE 码长上限 15
    if (lengths[i]) counts[lengths[i]]++;
  }
  // offsets[length]：该长度码字在 symbols 中的起始序号
  uint16_t offs[17];
  offs[0] = 0; offs[1] = 0;
  for (int l = 1; l <= 15; l++) offs[l + 1] = offs[l] + counts[l];
  for (int sym = 0; sym < n; sym++) {
    uint8_t l = lengths[sym];
    if (l) symbols[offs[l]++] = (uint16_t)sym;
  }
  return true;
}

// ---------- 逐位累积解码 ----------
int Inflate::decodeSym(const uint16_t* counts, const uint16_t* symbols, int maxLen) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len <= maxLen; len++) {
    code |= (int)_bitRead(1);
    if (_bad) return -1;
    int n = counts[len];
    if (code - first < n) return symbols[index + (code - first)];
    index += n;
    first = (first + n) << 1;
    code <<= 1;
  }
  fail("非法 Huffman 码");
  return -1;
}

// ---------- 输出 ----------
void Inflate::put(uint8_t b) {
  if (_outCap && _outLen >= _outCap) { fail("输出缓冲超出预设上限"); return; }
  _out->concat((const char*)&b, 1);
  _outLen++;
}

void Inflate::putCopy(uint32_t dist, uint32_t len) {
  // 距离必须落在已输出范围内，否则是损坏数据（防越界读）
  if (dist > _outLen) { fail("距离超出已输出长度（数据损坏）"); return; }
  size_t start = _out->length() - dist;
  for (uint32_t k = 0; k < len; k++) {
    if (_bad) return;
    // 注意：必须逐字节取，不能整段 memcpy —— 相邻匹配会自我重叠（如 dist=1）
    uint8_t b = (uint8_t)(*_out)[start + k];
    put(b);
  }
}

// ---------- 一个压缩块 ----------
bool Inflate::block(const uint16_t* lc, const uint16_t* ls,
                    const uint16_t* dc, const uint16_t* ds) {
  for (;;) {
    int s = decodeSym(lc, ls, 15);
    if (_bad) return false;
    if (s == 256) return true;                       // end of block
    if (s < 0) return false;
    if (s < 256) { put((uint8_t)s); continue; }      // literal
    int i = s - 257;
    if (i >= 29) { fail("长度码越界"); return false; }
    uint32_t length = LEN_BASE[i] + _bitRead(LEN_EXTRA[i]);
    int d = decodeSym(dc, ds, 15);
    if (_bad) return false;
    if (d < 0 || d >= 30) { fail("距离码越界"); return false; }
    uint32_t dist = DIST_BASE[d] + _bitRead(DIST_EXTRA[d]);
    putCopy(dist, length);
    if (_bad) return false;
  }
}

// ---------- CRC32（RFC 1952 要求，自带实现——本工具链无 zlib）----------
static uint32_t crc32Calc(const uint8_t* data, size_t len) {
  static uint32_t tbl[256];
  static bool inited = false;
  if (!inited) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      tbl[i] = c;
    }
    inited = true;
  }
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; i++) crc = tbl[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFU;
}

// ---------- 内部主循环（gzip 或 raw 共用）----------
bool Inflate::inflateCore(bool isGzip, String& out) {
  // gzip 头（RFC 1952）：ID1 ID2 CM FLG MTIME[4] XFL OS
  if (isGzip) {
    if (_srclen < 18) { fail("gzip 数据过短"); return false; }
    if (_src[0] != 0x1F || _src[1] != 0x8B) { fail("gzip 魔数不符"); return false; }
    if (_src[2] != 8) { fail("gzip 仅支持 deflate 压缩方法"); return false; }
    uint8_t flg = _src[3];
    size_t p = 10;
    if (flg & 4) {                                     // FEXTRA
      if (p + 2 > _srclen) { fail("FEXTRA 越界"); return false; }
      uint16_t xlen = (uint16_t)_src[p] | ((uint16_t)_src[p + 1] << 8);
      p += 2 + xlen;
    }
    if (flg & 8) { while (p < _srclen && _src[p]) p++; p++; }   // FNAME
    if (flg & 16) { while (p < _srclen && _src[p]) p++; p++; }  // FCOMMENT
    if (flg & 2) p += 2;                              // FHCRC
    if (p > _srclen) { fail("gzip 头越界"); return false; }
    _pos = p;
    _bit = 0;
  }

  // fixed Huffman 表（RFC 1951 §3.2.6）
  uint8_t fixLit[288];
  int i = 0;
  for (; i < 144; i++) fixLit[i] = 8;
  for (; i < 256; i++) fixLit[i] = 9;
  for (; i < 280; i++) fixLit[i] = 7;
  for (; i < 288; i++) fixLit[i] = 8;
  uint8_t fixDist[30];
  for (i = 0; i < 30; i++) fixDist[i] = 5;
  uint16_t fixLitC[16], fixLitS[288], fixDistC[16], fixDistS[30];
  if (!buildHuff(fixLit, 288, fixLitC, fixLitS)) { fail("fixed 表构建失败"); return false; }
  if (!buildHuff(fixDist, 30, fixDistC, fixDistS)) { fail("fixed 表构建失败"); return false; }

  // 块循环
  for (;;) {
    uint32_t bfinal = _bitRead(1);
    uint32_t btype = _bitRead(2);
    if (_bad) return false;
    if (btype == 0) {                                  // stored
      alignByte();
      if (_pos + 4 > _srclen) { fail("stored 块头越界"); return false; }
      uint16_t len = (uint16_t)_src[_pos] | ((uint16_t)_src[_pos + 1] << 8);
      uint16_t nlen = (uint16_t)_src[_pos + 2] | ((uint16_t)_src[_pos + 3] << 8);
      _pos += 4;
      if ((uint16_t)~len != nlen) { fail("stored 块 LEN/NLEN 不匹配"); return false; }
      if (_pos + len > _srclen) { fail("stored 数据越界"); return false; }
      for (uint16_t k = 0; k < len; k++) put(_src[_pos + k]);
      _pos += len;
    } else if (btype == 1) {                           // fixed Huffman
      if (!block(fixLitC, fixLitS, fixDistC, fixDistS)) return false;
    } else if (btype == 2) {                           // dynamic Huffman
      uint32_t hlit = _bitRead(5) + 257;
      uint32_t hdist = _bitRead(5) + 1;
      uint32_t hclen = _bitRead(4) + 4;
      if (_bad) return false;
      if (hlit > 286 || hdist > 30) { fail("动态表规模越界"); return false; }
      uint8_t clen[19];
      for (i = 0; i < 19; i++) clen[i] = 0;
      for (i = 0; i < (int)hclen; i++) clen[CLEN_ORDER[i]] = (uint8_t)_bitRead(3);
      if (!buildHuff(clen, 19, _clCounts, _clSymbols)) {
        fail("码长表构建失败");
        return false;
      }
      uint8_t lens[320];
      int total = (int)(hlit + hdist);
      int n = 0;
      while (n < total) {
        int s = decodeSym(_clCounts, _clSymbols, 15);
        if (_bad || s < 0) return false;
        if (s < 16) {
          lens[n++] = (uint8_t)s;
        } else if (s == 16) {                          // 重复上一个码长 3-6 次
          if (n == 0) { fail("码长重复码但无前值"); return false; }
          uint8_t prev = lens[n - 1];
          uint32_t r = 3 + _bitRead(2);
          while (r-- && n < total) lens[n++] = prev;
        } else if (s == 17) {                          // 0 重复 3-10 次
          uint32_t r = 3 + _bitRead(3);
          while (r-- && n < total) lens[n++] = 0;
        } else {                                      // 18: 0 重复 11-138 次
          uint32_t r = 11 + _bitRead(7);
          while (r-- && n < total) lens[n++] = 0;
        }
        if (_bad) return false;
      }
      if (!buildHuff(lens, (int)hlit, _litCounts, _litSymbols) ||
          !buildHuff(lens + hlit, (int)hdist, _distCounts, _distSymbols)) {
        fail("动态字面/距离表构建失败");
        return false;
      }
      if (!block(_litCounts, _litSymbols, _distCounts, _distSymbols)) return false;
    } else {
      fail("保留的块类型 btype=3");
      return false;
    }
    if (_bad) return false;
    if (bfinal) break;
  }

  // gzip 尾部校验：CRC32 与 ISIZE（各 4 字节，小端）
  // ⚠️ 必须从**缓冲区末尾**读，不能用 _pos：位读取结束时 _pos 可能因末字节
  //    未用尽而偏差若干字节（实测曾因此 CRC 误判不通过）。
  if (isGzip) {
    if (_srclen < 8) { fail("gzip 尾部缺失"); return false; }
    const uint8_t* t = _src + (_srclen - 8);
    uint32_t crcGot = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                      ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
    uint32_t sizeGot = (uint32_t)t[4] | ((uint32_t)t[5] << 8) |
                       ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
    uint32_t crcCalc = crc32Calc((const uint8_t*)out.c_str(), out.length());
    if (crcCalc != crcGot) {
      // 注意：fail() 保存的是指针，故缓冲必须 static（不能是栈上局部数组，否则悬空）
      static char msg[96];
      snprintf(msg, sizeof(msg), "CRC32 mismatch: got %08X, file %08X, out %u B",
               (unsigned)crcCalc, (unsigned)crcGot, (unsigned)out.length());
      fail(msg);
      return false;
    }
    if (sizeGot != (out.length() & 0xFFFFFFFFUL)) {
      static char msg2[72];
      snprintf(msg2, sizeof(msg2), "ISIZE mismatch: file %u, out %u",
               (unsigned)sizeGot, (unsigned)out.length());
      fail(msg2);
      return false;
    }
  }
  return true;
}

// ---------- 对外入口 ----------
const char* Inflate::_lastErr = "";
uint32_t    Inflate::_limit  = 0;

const char* Inflate::lastError() { return _lastErr ? _lastErr : "未知错误"; }

bool Inflate::gunzip(const uint8_t* src, size_t srclen, String& out) {
  out = "";
  Inflate z;
  _lastErr = "";
  z._src = src; z._srclen = srclen; z._out = &out; z._outCap = _limit;
  bool ok = z.inflateCore(true, out);
  if (!ok) out = "";                // 失败不留半截数据，避免上层解析到残缺 JSON
  return ok;
}

bool Inflate::inflateRaw(const uint8_t* src, size_t srclen, String& out) {
  out = "";
  Inflate z;
  _lastErr = "";
  z._src = src; z._srclen = srclen; z._out = &out; z._outCap = _limit;
  bool ok = z.inflateCore(false, out);
  if (!ok) out = "";
  return ok;
}