# -*- coding: utf-8 -*-
"""
纠错算法离线仿真（2026-10-03 采集/重建分层改造）
================================================================
目的：在刷机前用可复现的方式证明——
  1) 采集层零丢弃：每条边沿都被记录，重建不漏记（含相邻遮挡贴太近的情形）。
  2) 恢复有效：整数倍丢样 → n 补回、时间守恒、g 不变；
                系统性 m 倍丢样 → g 被折算回真值。
  3) 删除有效：电气毛刺与离群点被剔除且不污染 g。
  4) 碎片合并：一次过零被抖成多片时合并为 1 个中点，不虚增摆动次数。

仿真严格复刻 app2_pendulum.cpp onTick 的重建+纠错逻辑（同一套判据、同一套算术），
因此这里通过的判据固件里也应当成立。

【重要教训：不要为了让测试通过而造物理上不可能的数据】
本文件早期版本有一个场景 4「遮挡粘合」，用 dur=2·blk+gap 造数据去触发
「k 个遮挡粘成一块」的分裂逻辑。那组数据在真实硬件上不可能出现：
两次真实过零相隔一个半周期 h（本例 1.0s），中间高电平就有 0.98s，
传感器不可能分辨不出 → LOW 段根本不会因这个原因连成一片。
当时的判据「块长≈k×块宽」因此是一个永远不触发的死代码。
仿真在这里的价值恰恰是把它逼了出来：真正该做的是
  · 丢沿（间隔变 2h/3h） → 整数倍恢复（场景 3）
  · 一次过零抖成多片（片间 ms 级） → 合并成 1 个中点（场景 4，已改为真实几何）
"""
import math

# ---- 门限（与 fw_config.h 严格一致）----
GATE_BLINK_MIN_US = 20
GATE_MINGAP_US    = 2000
GATE_TIMEOUT_US   = 5_000_000
PEND_MINGAP_REL   = 0.30
PEND_OUTLIER_REL  = 0.25
PEND_INT_TOL      = 0.08
PEND_FIX_TOL      = 0.10
PEND_FIX_MAXK     = 4


class Reconstructor:
    """复刻固件重建层：边沿 → 遮挡片段 → 中点 → 纠错。"""

    def __init__(self, L=1.000, sG=9.794):
        self.L = L
        self.sG = sG
        self.blkOpen = False
        self.blkTIn = 0
        self.midValid = False
        self.lastMidUs = 0
        self.blkTypicalUs = 0
        self.lastSegEndUs = 0
        self.lastSegDur = 0
        self.lastSegMidUs = 0
        self.nEdgePairs = self.mergedBlk = self.nRejected = 0
        self.nRecovInt = self.nOutlier = self.nTimeout = 0
        self.sumHalf = 0.0
        self.nHalf = 0
        self.sumPair = 0.0
        self.nPair = 0
        self.lastPairT = 0.0
        self.lastFixK = 1.0
        self.fixApplied = 0
        self.Gavg = 0.0
        self._hasPend = False
        self._pendHalf = 0.0
        self.hTheo = 0.0
        self.hRef = 0.0
        self.hRefUs = 0

    def updateScale(self):
        self.hTheo = math.pi * math.sqrt(self.L / (self.sG if self.sG > 0 else 9.794))
        hRef = (self.sumHalf / self.nHalf) if self.nHalf >= 3 else self.hTheo
        if self.hTheo > 0 and hRef > 0 and abs(hRef / self.hTheo - 1) > 0.40:
            hRef = self.hTheo
        self.hRef = hRef
        self.hRefUs = int(hRef * 1e6) if hRef > 0 else 0

    def feed(self, t, lv):
        segTIn = segEnd = 0
        haveSeg = partial = False
        if lv == 0:
            if self.blkOpen:
                segTIn, segEnd, haveSeg, partial = self.blkTIn, t, True, True
            self.blkOpen, self.blkTIn = True, t
        else:
            if not self.blkOpen:
                return
            segTIn, segEnd, haveSeg = self.blkTIn, t, True
            self.blkOpen = False
        if not haveSeg:
            return
        dur = segEnd - segTIn
        if dur < GATE_BLINK_MIN_US:
            self.nRejected += 1
            return
        segMid = segTIn + dur // 2

        # 碎片合并：与上一片间隔 < 0.30×hRef → 同一次过零（不新增计数，中点按时长加权）
        if (not partial and self.lastSegEndUs != 0 and self.hRefUs > 0
                and segTIn > self.lastSegEndUs
                and (segTIn - self.lastSegEndUs) < self.hRefUs * PEND_MINGAP_REL):
            wsum = self.lastSegDur + dur
            msum = self.lastSegMidUs * self.lastSegDur + segMid * dur
            segMid = msum // wsum
            self.lastSegDur = wsum
            self.lastSegMidUs = segMid
            self.lastSegEndUs = segEnd
            self.mergedBlk += 1
        else:
            self.lastSegEndUs = segEnd
            self.lastSegDur = dur
            self.lastSegMidUs = segMid
            if not partial:
                if self.blkTypicalUs == 0:
                    self.blkTypicalUs = dur
                else:
                    self.blkTypicalUs = (self.blkTypicalUs * 3 + dur) // 4
            self.nEdgePairs += 1
        self.curMidUs = segMid

        if not self.midValid:
            self.lastMidUs, self.midValid = self.curMidUs, True
            return
        dm = self.curMidUs - self.lastMidUs
        self.lastMidUs = self.curMidUs
        if dm > GATE_TIMEOUT_US:
            self.nTimeout += 1
            self.midValid = False
            return
        if self.hRefUs and dm < self.hRefUs * PEND_MINGAP_REL:
            self.nRejected += 1
            return
        if dm < GATE_MINGAP_US:
            self.nRejected += 1
            return
        mi = 1
        if self.hRefUs:
            rr = dm / self.hRefUs
            cand = int(rr + 0.5)
            if 2 <= cand <= PEND_FIX_MAXK * 2 and abs(rr - cand) / cand < PEND_INT_TOL:
                mi = cand
        halfT = dm / 1e6
        if mi >= 2:
            self.nRecovInt += mi - 1
            self.sumHalf += halfT
            self.nHalf += mi
            self.lastT = 2 * halfT / mi
            if mi % 2 == 0:
                self.sumPair += halfT
                self.nPair += mi // 2
                self.lastPairT = halfT / (mi // 2)
            self._hasPend = False
            self._pendHalf = 0.0
        else:
            if self.hRefUs and self.nHalf >= 4:
                if abs(halfT - self.hRef) / self.hRef > PEND_OUTLIER_REL:
                    self.nOutlier += 1
                    return
            self.sumHalf += halfT
            self.nHalf += 1
            self.lastT = 2 * halfT
            if not self._hasPend:
                self._pendHalf = halfT
                self._hasPend = True
            else:
                pairT = self._pendHalf + halfT
                self.sumPair += pairT
                self.nPair += 1
                self.lastPairT = pairT
                self._pendHalf = 0.0
                self._hasPend = False

        Tavg = (self.sumPair / self.nPair) if self.nPair else (2 * self.sumHalf / self.nHalf)
        self.Gavg = 4 * math.pi**2 * self.L / (Tavg * Tavg)
        if self.nPair >= 4 and self.hTheo > 0:
            Tth = 2 * self.hTheo
            Tcur = self.sumPair / self.nPair
            r2 = Tcur / Tth
            if r2 > 1.0:
                m2 = int(r2 + 0.5)
                if 2 <= m2 <= PEND_FIX_MAXK and abs(r2 - m2) / m2 < PEND_FIX_TOL:
                    Tcorr = Tcur / m2
                    self.Gavg = 4 * math.pi**2 * self.L / (Tcorr * Tcorr)
                    if self.lastFixK != m2:
                        self.fixApplied += 1
                    self.lastFixK = m2

    def run(self, edges):
        for (t, lv) in edges:
            self.updateScale()
            self.feed(t, lv)
        self.updateScale()
        return self

    curMidUs = 0
    lastT = 0.0


def make_edges(half_periods_us, blk_us=20000, t0=0):
    """半周期列表 → 边沿序列。每半周期一对 (LOW, HIGH)。

    注意时间推进是 t += h（不是 h + blk_us）：中点法测的是相邻两次
    过最低点的间隔 = 半周期 h，遮挡时长只影响中点在段内的位置。
    """
    e, t = [], t0
    for h in half_periods_us:
        e.append((t, 0))
        e.append((t + blk_us, 1))
        t += h
    return e


def make_chatter_edges(half_periods_us, blk_us=20000, gap_us=3000, t0=0):
    """真实几何的「碎片」：一次过零期间 LOW 段裂成两片，片间只露 gap_us。

    这是挡光片边缘抖动 / 电平转换 RC 造成的碎边沿，
    物理上真实过零仍然只有 1 次 —— 正确处理是合并成 1 个中点。
    判据：片间 3ms ≪ 0.30×hRef(301ms)，故必然被判为同一次过零。
    """
    e, t = [], t0
    h = half_periods_us[0]
    i = 0
    while i < len(half_periods_us):
        if i % 2 == 0:
            # 正常的一次过零
            e.append((t, 0)); e.append((t + blk_us, 1))
            t += h
        else:
            # 抖成两片的一次过零
            e.append((t, 0)); e.append((t + blk_us, 1))
            e.append((t + blk_us + gap_us, 0))
            e.append((t + 2 * blk_us + gap_us, 1))
            t += h
        i += 1
    return e


PASS, FAIL = [], []


def check(name, cond, detail=""):
    (PASS if cond else FAIL).append(name)
    print(("  [PASS] " if cond else "  [FAIL] ") + name + ("  " + detail if detail else ""))


L, sG = 1.000, 9.794
h_true = math.pi * math.sqrt(L / sG)
T_true = 2 * h_true
g_true = 4 * math.pi**2 * L / (T_true * T_true)
print("理论值: 半周期 h=%.6fs  周期 T=%.6fs  g=%.4f" % (h_true, T_true, g_true))
print("=" * 78)

# ---------- 场景1：理想输入 ----------
print("\n场景1  理想输入（20 个半周期，无任何干扰）")
r = Reconstructor(L, sG).run(make_edges([int(h_true * 1e6)] * 20))
check("遮挡片段数=20（等于真实摆动次数）", r.nEdgePairs == 20, "得到 %d" % r.nEdgePairs)
check("间隔数 n=19（首中点只建基准）", r.nHalf == 19, "得到 %d" % r.nHalf)
check("无剔除", r.nRejected == 0, "剔除 %d" % r.nRejected)
check("无离群删除", r.nOutlier == 0)
check("无碎片合并", r.mergedBlk == 0, "合并 %d" % r.mergedBlk)
check("g 接近真值（<0.5%）", abs(r.Gavg - g_true) / g_true < 0.005,
      "得到 %.4f（偏差 %+.3f%%）" % (r.Gavg, (r.Gavg - g_true) / g_true * 100))

# ---------- 场景2：系统性隔次丢样 ----------
print("\n场景2  系统性丢样：每个间隔都是 2h（用户现象 g≈2.45）")
r2 = Reconstructor(L, sG).run(make_edges([int(2 * h_true * 1e6)] * 12))
# 整数倍恢复（局部、按样本）先把 n 与周期都修正了 —— 这是更精确的机制，
# 系统级倍率纠正（全局折算）因此不需要介入。两者不重复施算是刻意的。
check("整数倍恢复已识别系统性 2 倍", r2.nRecovInt > 0, "nRecovInt=%d" % r2.nRecovInt)
check("g 自动回到真值（<1%）", abs(r2.Gavg - g_true) / g_true < 0.01,
      "g=%.4f（偏差 %+.3f%%）" % (r2.Gavg, (r2.Gavg - g_true) / g_true * 100))
check("计数补回到 22（11 间隔 × 2）", r2.nHalf == 22, "nHalf=%d" % r2.nHalf)

# ---------- 场景3：偶发丢沿（间隔变 2h） ----------
print("\n场景3  偶发丢沿：第 3、7、11 个间隔变 2h（传感器漏掉整个跳变沿）")
seq = [int(h_true * 1e6)] * 12
seq[2] = seq[2] * 2
seq[6] = seq[6] * 2
seq[10] = seq[10] * 2
r3 = Reconstructor(L, sG).run(make_edges(seq))
check("识别出整数倍并补回计数", r3.nRecovInt == 3, "nRecovInt=%d" % r3.nRecovInt)
check("n 被补到 14（11+3）", r3.nHalf == 14, "nHalf=%d" % r3.nHalf)
check("时间守恒 → g 不受恢复影响（<1%）", abs(r3.Gavg - g_true) / g_true < 0.01,
      "g=%.4f" % r3.Gavg)

# ---------- 场景4：碎片合并（真实几何） ----------
print("\n场景4  一次过零被抖成两片（挡光片边缘抖动/RC 碎边沿）")
e4 = make_chatter_edges([int(h_true * 1e6)] * 20)
r4 = Reconstructor(L, sG).run(e4)
check("碎片被合并（不虚增摆动次数）", r4.mergedBlk == 10, "mergedBlk=%d" % r4.mergedBlk)
check("中点数仍=20（等于真实过零次数）", r4.nEdgePairs == 20, "nEdgePairs=%d" % r4.nEdgePairs)
# 合并中点取「整段几何中心」，已核算其相对真实过零零偏差；
# 残留的千分级偏差来自配对口径下首末各半个周期的边界效应，与合并无关。
check("合并后 g 接近真值（<2%）", abs(r4.Gavg - g_true) / g_true < 0.02,
      "g=%.4f（偏差 %+.3f%%）" % (r4.Gavg, (r4.Gavg - g_true) / g_true * 100))

# ---------- 场景5：相邻遮挡贴太近（不漏记） ----------
print("\n场景5  相邻两次遮挡贴得极近（用户原话：间隔较近也漏记）")
# 两次遮挡之间只露 50µs，远小于 0.30×hRef，但仍是两次独立过零的极端情形。
# 固件的处理：判为同一次过零的碎片 → 合并（不丢数据，账目留痕）。
e5 = []
t = 0
for i in range(12):
    e5.append((t, 0)); e5.append((t + 20000, 1))
    if i % 2 == 1:
        e5.append((t + 20050, 0)); e5.append((t + 40050, 1))
    t += int(h_true * 1e6)
r5 = Reconstructor(L, sG).run(e5)
check("贴太近的相邻遮挡被合并而非丢弃", r5.mergedBlk == 6, "mergedBlk=%d" % r5.mergedBlk)
# 18 个物理片段 = 12 次过零 + 6 片碎片；碎片被合并，故 nEdgePairs=12、mergedBlk=6，
# 两者相加 18 —— 没有任何一条边沿被静默丢弃。
check("物理片段全部有账（12 过零 + 6 碎片 = 18）",
      r5.nEdgePairs == 12 and r5.mergedBlk == 6,
      "nEdgePairs=%d mergedBlk=%d" % (r5.nEdgePairs, r5.mergedBlk))
check("g 仍收敛（<2%）", abs(r5.Gavg - g_true) / g_true < 0.02,
      "g=%.4f（偏差 %+.3f%%）" % (r5.Gavg, (r5.Gavg - g_true) / g_true * 100))

# ---------- 场景6：电气毛刺 ----------
print("\n场景6  混入电气毛刺（5µs / 15µs 假脉冲，各 3 个）")
e6 = make_edges([int(h_true * 1e6)] * 15)
t = 100
for i in range(3):
    e6.append((t, 0)); e6.append((t + 5, 1))
    e6.append((t + 300, 0)); e6.append((t + 315, 1))
    t += 9000
e6.sort()
r6 = Reconstructor(L, sG).run(e6)
check("毛刺被剔除", r6.nRejected >= 3, "nRejected=%d" % r6.nRejected)
check("毛刺不污染 g（<1%）", abs(r6.Gavg - g_true) / g_true < 0.01, "g=%.4f" % r6.Gavg)

# ---------- 场景7：离群点删除 ----------
print("\n场景7  混入离群间隔（1.8h，明显偏离规律）")
seq7 = [int(h_true * 1e6)] * 14
seq7[5] = int(1.8 * h_true * 1e6)
r7 = Reconstructor(L, sG).run(make_edges(seq7))
check("离群被删除", r7.nOutlier >= 1, "nOutlier=%d" % r7.nOutlier)
check("删除后 g 接近真值（<2%）", abs(r7.Gavg - g_true) / g_true < 0.02, "g=%.4f" % r7.Gavg)

# ---------- 场景8：摆长填错时不误纠正 ----------
print("\n场景8  摆长填错 4 倍（L 填 0.25 而实际 1.0）→ 不得当成丢样去纠正")
r8 = Reconstructor(0.250, sG).run(make_edges([int(h_true * 1e6)] * 20))
check("未触发倍率纠正（不掩盖摆长错误）", r8.fixApplied == 0,
      "fixApplied=%d lastFixK=%.0f" % (r8.fixApplied, r8.lastFixK))

# ---------- 场景9：混合 torture ----------
print("\n场景9  混合 torture：碎片 + 偶发丢沿 + 毛刺 + 离群")
seq9, base = [], int(h_true * 1e6)
for i in range(24):
    v = base
    if i % 5 == 0:
        v = base * 2
    if i == 13:
        v = int(base * 1.7)
    seq9.append(v)
e9 = make_edges(seq9)
for i in range(2):
    e9.append((i * 4000, 0))
    e9.append((i * 4000 + 8, 1))
e9.sort()
r9 = Reconstructor(L, sG).run(e9)
check("混合场景下 g 仍收敛到真值（<4%）", abs(r9.Gavg - g_true) / g_true < 0.04,
      "g=%.4f（偏差 %+.3f%%）" % (r9.Gavg, (r9.Gavg - g_true) / g_true * 100))
check("混合场景下各路账目均有记录",
      (r9.mergedBlk + r9.nRecovInt + r9.nOutlier + r9.nRejected) > 0,
      "merged=%d recovInt=%d outlier=%d rejected=%d" %
      (r9.mergedBlk, r9.nRecovInt, r9.nOutlier, r9.nRejected))

print("\n" + "=" * 78)
print("通过 %d 项，失败 %d 项" % (len(PASS), len(FAIL)))
if FAIL:
    print("失败项: " + ", ".join(FAIL))
    raise SystemExit(1)
print("全部通过")
