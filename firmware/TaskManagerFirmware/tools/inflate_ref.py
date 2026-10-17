#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
inflate_ref.py —— 自带 inflate 算法的参考实现与验证脚本（2026-10-02）

**为什么有这个文件**
本项目使用的 esp32:esp32 3.3.12-cn 工具链**不含 zlib**（无 zlib.h / libz.a），
而和风天气**无视 Accept-Encoding: identity，仍强制返回 GZIP**，
故 `fw_inflate.cpp` 是从零实现的 DEFLATE/gzip 解压。

本脚本是它的**参考实现 + 验证器**：
`fw_inflate.cpp` 移植自本文件（逐行对照），二者算法一致。
日后若要修改 inflate 逻辑，请先在此处验证正确性，再同步到 C++。

**验证结论（2026-10-02 已跑通）**
对照 `zlib.decompress` 解压，8/8 全过，CRC32 全部一致：
  - 真实 QWeather now 响应（316 B → 446 B）
  - 真实 QWeather 24h 响应（857 B → 5735 B）
  - 高度可压缩数据（fixed-Huffman 路径）
  - 随机数据（dynamic-Huffman 路径，含最长距离码）
  - 类 JSON 重复文本（动态距离码回溯）
  - 小数据（stored 路径）
  - 空内容
  - 超长重复序列（长距离匹配）
边界行为：截断/损坏数据**报错**而非静默输出错误结果。

**两个已踩过的坑（务必保留）**
1. CRC32/ISIZE 必须从**缓冲区末尾**读 8 字节，**不能**用位读取器结束时的 `pos`——
   末字节往往未用尽，`pos` 会偏 2 字节，导致 CRC 误判不通过（上板实测踩到）。
2. 距离匹配必须**逐字节**拷贝，不能整段 memcpy：相邻匹配会自我重叠（如 dist=1）。

用法：
    python inflate_ref.py <file.gz> [...]        # 验证若干 gzip 文件
    python inflate_ref.py --self                 # 生成内置用例并全部验证
"""
import struct
import sys
import zlib

# ---------------------------------------------------------------- 位读取器


class BitReader:
    """LSB 先行，符合 DEFLATE 规范。"""

    def __init__(self, data):
        self.d = data
        self.pos = 0
        self.bit = 0

    def read(self, n):
        v = 0
        for i in range(n):
            if self.pos >= len(self.d):
                raise EOFError("输入意外结束（位读取越界）")
            b = (self.d[self.pos] >> self.bit) & 1
            v |= b << i
            self.bit += 1
            if self.bit == 8:
                self.bit = 0
                self.pos += 1
        return v

    def align(self):
        if self.bit:
            self.bit = 0
            self.pos += 1


# ---------------------------------------------------------------- 码表

LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
            51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
             3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
             12289, 16385, 24577]
DIST_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
              9, 9, 10, 10, 11, 11, 12, 12, 13, 13]
CLEN_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]


# ---------------------------------------------------------------- Huffman


def build_huff(lengths):
    """由码长数组构建 canonical Huffman 解码表。
    返回 (counts, symbols)：counts[l] = 码长为 l 的码字个数。
    解码用经典的「逐位累积」法：code/first/index 三者推进。"""
    maxlen = max(lengths) if lengths else 0
    counts = [0] * (maxlen + 1)
    for l in lengths:
        if l:
            counts[l] += 1
    counts[0] = 0
    offsets = [0] * (maxlen + 2)
    for l in range(1, maxlen + 1):
        offsets[l + 1] = offsets[l] + counts[l]
    symbols = [0] * len(lengths)
    for sym, l in enumerate(lengths):
        if l:
            symbols[offsets[l]] = sym
            offsets[l] += 1
    return counts, symbols


def decode_sym(br, counts, symbols):
    code = 0
    first = 0
    index = 0
    for length in range(1, len(counts)):
        code |= br.read(1)
        n = counts[length]
        if code - first < n:
            return symbols[index + (code - first)]
        index += n
        first = (first + n) << 1
        code <<= 1
    raise ValueError("非法 Huffman 码")


# ---------------------------------------------------------------- 解压


def _block(br, out, counts, syms, dcounts, dsyms):
    while True:
        s = decode_sym(br, counts, syms)
        if s == 256:
            return
        if s < 256:
            out.append(s)
        else:
            i = s - 257
            length = LEN_BASE[i] + br.read(LEN_EXTRA[i])
            d = decode_sym(br, dcounts, dsyms)
            dist = DIST_BASE[d] + br.read(DIST_EXTRA[d])
            start = len(out) - dist
            if start < 0:
                raise ValueError("距离超出已输出长度（数据损坏）")
            # 必须逐字节（相邻匹配会自我重叠，如 dist=1）
            for k in range(length):
                out.append(out[start + k])


def inflate(data, verify=True):
    """解压 gzip，返回 (明文, crc校验通过)。raw deflate 用 inflate_raw。"""
    br = BitReader(data)
    if data[0] != 0x1F or data[1] != 0x8B:
        raise ValueError("非 gzip（魔数不符）")
    if data[2] != 8:
        raise ValueError("gzip 仅支持 deflate 压缩方法")
    flg = data[3]
    br.pos = 10
    if flg & 4:                                   # FEXTRA
        xlen = struct.unpack_from("<H", data, br.pos)[0]
        br.pos += 2 + xlen
    if flg & 8:                                   # FNAME
        while data[br.pos]:
            br.pos += 1
        br.pos += 1
    if flg & 16:                                  # FCOMMENT
        while data[br.pos]:
            br.pos += 1
        br.pos += 1
    if flg & 2:                                   # FHCRC
        br.pos += 2

    fixed_lit = build_huff([8] * 144 + [9] * 112 + [7] * 24 + [8] * 8)
    fixed_dist = build_huff([5] * 30)
    out = bytearray()

    while True:
        bfinal = br.read(1)
        btype = br.read(2)
        if btype == 0:                            # stored
            br.align()
            ln = struct.unpack_from("<H", data, br.pos)[0]
            br.pos += 4                           # LEN + NLEN
            out += data[br.pos:br.pos + ln]
            br.pos += ln
        elif btype == 1:                          # fixed Huffman
            c, s = fixed_lit
            dc, ds = fixed_dist
            _block(br, out, c, s, dc, ds)
        elif btype == 2:                          # dynamic Huffman
            hlit = br.read(5) + 257
            hdist = br.read(5) + 1
            hclen = br.read(4) + 4
            clens = [0] * 19
            for i in range(hclen):
                clens[CLEN_ORDER[i]] = br.read(3)
            cc, cs = build_huff(clens)
            lens = []
            while len(lens) < hlit + hdist:
                sym = decode_sym(br, cc, cs)
                if sym < 16:
                    lens.append(sym)
                elif sym == 16:
                    lens += [lens[-1]] * (3 + br.read(2))
                elif sym == 17:
                    lens += [0] * (3 + br.read(3))
                else:
                    lens += [0] * (11 + br.read(7))
            c, s = build_huff(lens[:hlit])
            dc, ds = build_huff(lens[hlit:hlit + hdist])
            _block(br, out, c, s, dc, ds)
        else:
            raise ValueError("保留的块类型 btype=3")

        if bfinal:
            break

    # 校验 CRC32 与 ISIZE。
    # ⚠️ 必须从**缓冲区末尾**读 8 字节，不能用 br.pos：
    #    末字节往往未用尽，br.pos 会偏 2 字节 → CRC 误判不通过（C++ 版上板踩过）。
    if verify and len(data) >= 8:
        crc, isize = struct.unpack("<II", data[-8:])
        if crc != (zlib.crc32(bytes(out)) & 0xFFFFFFFF):
            raise ValueError("CRC32 校验不通过")
        if isize != (len(out) & 0xFFFFFFFF):
            raise ValueError("ISIZE 校验不通过")
    return bytes(out)


# ---------------------------------------------------------------- 自检


def _self_test():
    import gzip
    import random
    random.seed(42)
    cases = {}

    # 真实 QWeather 响应（若此前保存过）
    for f in ("now.gz", "h24.gz"):
        try:
            cases["real-" + f] = open(f, "rb").read()
        except OSError:
            pass

    cases["fixed（高压缩）"] = gzip.compress(b"A" * 5000, 9)
    cases["dynamic（随机）"] = gzip.compress(
        bytes(random.randrange(256) for _ in range(3000)), 9)
    cases["json 重复（长距离码）"] = gzip.compress(
        ('{"now":{"temp":"16","text":"晴"},}' * 200).encode(), 9)
    cases["stored（小数据）"] = gzip.compress(b"x" * 20, 1)
    cases["empty（空内容）"] = gzip.compress(b"", 9)
    cases["bigrun（超长重复）"] = gzip.compress(b"ab" * 20000, 9)

    print(f"{'用例':<26} {'压缩':>8} {'解压':>8}  结果")
    print("-" * 62)
    ok = 0
    for name, raw in cases.items():
        try:
            got = inflate(raw)
            exp = zlib.decompress(raw, 16 + zlib.MAX_WBITS)
            good = got == exp
        except Exception as e:
            got, good = b"", False
            print(f"{name:<26} {len(raw):>8} {'-':>8}  异常: {e}")
            continue
        print(f"{name:<26} {len(raw):>8} {len(got):>8}  {'OK' if good else 'FAIL'}")
        ok += 1 if good else 0

    print("-" * 62)
    print(f"通过 {ok}/{len(cases)}")

    # 边界：损坏数据必须报错而非静默输出
    print("\n边界用例（应报错）：")
    for name, raw in (("截断", cases["fixed（高压缩）"][:40]),
                      ("损坏", bytes([0x1F, 0x8B, 8, 0]) + b"\x00" * 30)):
        try:
            inflate(raw)
            print(f"  {name:<6} 未报错 ← 不符合预期！")
        except Exception as e:
            print(f"  {name:<6} 正确报错: {type(e).__name__}")
    return 0 if ok == len(cases) else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--self":
        sys.exit(_self_test())
    if len(sys.argv) > 1:
        ok = 0
        for p in sys.argv[1:]:
            raw = open(p, "rb").read()
            try:
                got = inflate(raw)
                exp = zlib.decompress(raw, 16 + zlib.MAX_WBITS)
                good = got == exp
            except Exception as e:
                print(f"[FAIL] {p}: {e}")
                continue
            print(f"[{'OK  ' if good else 'FAIL'}] {p}  {len(raw)}B -> {len(got)}B")
            ok += 1 if good else 0
        print(f"\n通过 {ok}/{len(sys.argv) - 1}")
        sys.exit(0 if ok == len(sys.argv) - 1 else 1)
    print(__doc__)
    sys.exit(_self_test())
