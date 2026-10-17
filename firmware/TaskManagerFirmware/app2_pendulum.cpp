// =====================================================================
// app2_pendulum.cpp —— 程序2「单摆测g」实现
// 计时法：中点法（02 分册修订补丁，对齐原版 lsw.rs）——
//   遮挡时段中点 = 摆球过最低点时刻，相邻中点差 = 半周期。
//
// 2026-10-03 采集/重建分层改造（用户要求：极致化灵敏 + 自带纠错）
// ---------------------------------------------------------------------
// 改造前：ISR 里一边记时间戳一边做三件事（配对、算中点、门限判决并丢弃）。
//   后果是「丢样不可逆」——门限一旦杀掉样本，loop 层拿不到任何信息，
//   只能事后从 n 与 g 的偏差里猜，既无法恢复也无法定位。
// 改造后：
//   ISR  = 纯采集。每条边沿（亮/灭）都带时间戳入队，零判决、零丢弃。
//   loop = 纯重建 + 全部纠错。见 rebuildFromEdges()。
// 因此「相邻两次遮挡贴太近被并成一个块」「电气毛刺」「丢了一半」
// 这些以前在采集层被静默吞掉的情形，现在都能被识别、分裂、补回。
// =====================================================================
#include "app2_pendulum.h"
#include "fw_config.h"
#include <math.h>
#include "fw_store.h"
#include "fw_display.h"
#include "fw_time.h"
#include "fw_manager.h"
#include "fw_ble.h"
#include "fw_net.h"

// 真实运行的实例在 TaskManagerFirmware.ino 中定义（静态对象），此处不另建副本。

// ===================== 采集层：原始边沿环形队列 =====================
// SPSC：ISR 是唯一生产者（每条边沿写一条），loop 是唯一消费者。
// 存的是「边沿」不是「样本」—— 这是能自动纠错的前提：
// 只要边沿还在，任何一次误判都可以在 loop 里推翻重来。
#define EDGE_RING      GATE_EDGE_RING
static volatile uint32_t s_edgeUs[EDGE_RING];   // 边沿时刻（µs）
static volatile uint8_t  s_edgeLv[EDGE_RING];   // 0=LOW(进遮挡) 1=HIGH(出遮挡)
static volatile uint8_t  s_edgeHead = 0;       // 写游标（仅 ISR 改）
static volatile uint8_t  s_edgeTail = 0;       // 读游标（仅 loop 改）

// 采集层统计（只累加，不判决）
static volatile uint32_t s_edgeTotal  = 0;     // 采集到的边沿总数（含亮与灭）
static volatile uint32_t s_ovfTotal   = 0;     // 队列满导致的丢边沿（应恒为 0）

// ISR 内读 GPIO：优先直接读外设寄存器（不依赖 Flash 缓存，NVS 写期间也安全）。
// Arduino 的 digitalRead() 内部会调用 perimanGetPinBus()——该函数位于 Flash，
// 在系统写 Flash（本固件全过程频繁写 NVS）的 Cache 失效窗口内存在取指风险，
// 因此这里在可用时改用寄存器直读，否则回落 digitalRead。
#if __has_include(<soc/gpio_reg.h>)
  #include <soc/gpio_reg.h>
#endif
static inline IRAM_ATTR bool gateIsLow() {
#if defined(GPIO_IN_REG)
  return ((REG_READ(GPIO_IN_REG) >> PIN_OPTOGATE) & 0x1U) == 0U;   // 空闲高 / 遮挡低
#else
  return digitalRead(PIN_OPTOGATE) == LOW;
#endif
}

// GPIO4 中断：CHANGE 双沿。函数体已压到最小 —— 只读时间、读电平、写队列。
// 任何多余语句都会拉长中断占用时间，而中断占用正是丢边沿的直接原因。
static void IRAM_ATTR gateISR() {
  uint32_t t = micros();
  uint8_t  lv = gateIsLow() ? 0 : 1;
  uint8_t  nx = (uint8_t)((s_edgeHead + 1) & (EDGE_RING - 1));
  if (nx == s_edgeTail) {                       // 队列满：丢最旧并计数（不静默）
    s_edgeTail = (uint8_t)((s_edgeTail + 1) & (EDGE_RING - 1));
    s_ovfTotal = s_ovfTotal + 1;                // 不用 ++：volatile 上的 ++ 自 C++20 起废弃（-Wvolatile）
  }
  s_edgeUs[s_edgeHead] = t;
  s_edgeLv[s_edgeHead] = lv;
  s_edgeHead = nx;
  s_edgeTotal = s_edgeTotal + 1;
}

// ===================== 生命周期 =====================

void PendulumApp::onCreate() {
  _pendLen = Store.getDouble("app2.pend_len", PEND_LEN_DEFAULT);
  Store.getString("app2.sg_src", _sgSrc);
  _sG  = Store.getDouble("app2.sg_cache", SG_DEFAULT);
  _sgTs = (uint32_t)Store.getInt("app2.sg_ts", 0);
  _measuring = false;
  resetMeasure();

  // 当地标准 g：本地公式即时计算并缓存（分册02第5节）
  computeSg();

  // 注册本程序 BLE 菜单（总纲第 6 节：[2]摆长 [3]历史 由程序2注册）
  Ble.registerMenu("2", "摆长/波形", this);
  Ble.registerMenu("3", "历史数据", this);
  Serial.printf("[Pend] onCreate L=%.3f sG=%.3f(%s)\n", _pendLen, _sG, _sgSrc.c_str());
}

void PendulumApp::onShow() {
  // 挂载中断（活跃时才挂载，总纲 5.2）；重绘首屏
  pinMode(PIN_OPTOGATE, INPUT_PULLUP);   // 转换模块侧已有上拉，内部上拉仅作保险
  attachInterrupt(digitalPinToInterrupt(PIN_OPTOGATE), gateISR, CHANGE);
  noInterrupts();
  // 分两行写：链式赋值 s_edgeHead = s_edgeTail = 0 会被编译器判为
  // "using value of assignment with volatile-qualified operand"（-Wvolatile，C++20 起废弃）
  s_edgeTail = 0;
  s_edgeHead = 0;                     // 切走期间摆动的边沿不带入本次
  s_edgeTotal = 0;
  s_ovfTotal  = 0;
  interrupts();
  // 重建状态机是 loop 独占的普通成员，此处无需关中断
  _blkOpen = false;
  _midValid = false;
  _pendSegValid = false;   // 切走期间的 pending 不带入本次（与丢队列同语义）
  _psValid = false;        // R7b：暂存半途片一并清空
  _needDraw = true;
}
void PendulumApp::onHide() {
  // 注销中断；测量累计值保留 RAM → 长按切回可续测（分册02第1节）
  detachInterrupt(digitalPinToInterrupt(PIN_OPTOGATE));
  noInterrupts();
  s_edgeTail = 0; s_edgeHead = 0;     // 丢弃未消费边沿：切走期间的摆动不计入本次
  interrupts();
  _blkOpen = false;
  // 波形流：切走自动停止（中断已卸载，无新数据可推）
  if (_streaming) {
    _streaming = false;
    Ble.streamEnd();
  }
  // 导出：切走后本程序 onTick 停摆，分块发送永远无法推进 → PC 端永远等不到
  // #EXPORT END（而 ble_cli 导出期间还忽略键盘输入，表现为工具整体卡死）。
  // 因此切走时必须立刻收尾：补发 END 让 PC 正常落盘，并复位导出状态。
  if (_exporting) {
    _exporting = false;
    Ble.exportEnd();
    Ble.replyINFO("导出被中断（已切换程序），已发送 " + String(_exportIdx) + "/" + String(_exportCnt) + " 组");
  }
}

void PendulumApp::resetMeasure() {
  // 开始测量自动清零（总纲 1：原"长按复位"已废除）
  _sumHalf = 0; _nHalf = 0;
  _lastT = 0; _lastG = 0; _Gavg = 0;
  // 配对状态一并清零（否则上一轮的落单样本会混进下一轮的平均）
  _nPair = 0; _sumPair = 0; _pendHalf = 0; _hasPend = false;
  _altDetected = false; _altAmp = 0; _altCnt = 0; _nDropped = 0; _lastPairT = 0;
  // 重建层状态与统计一并清零
  _blkOpen = false; _midValid = false; _lastMidUs = 0; _blkTypicalUs = 0;
  _pendSegEndUs = 0; _pendSegDur = 0; _pendSegMidUs = 0; _pendSegValid = false;
  _psTIn = 0; _psEnd = 0; _psValid = false;
  _replayMid = 0; _hasReplay = false;
  _nEdgePairs = 0; _mergedBlk = 0; _nRejected = 0; _nRecovInt = 0;
  _nOutlier = 0; _nTimeout = 0; _lastFixK = 1.0; _fixApplied = 0;
  _nStuck = 0;
  _degraded = true;   // R7c 修订：默认宽容（重开测量不回到严格模式）
  _pendShort = 0; _hasShort = false; _nShortDrop = 0; _outlierWarned = false;
  noInterrupts();
  // 边沿队列清空：与 ISR 共享，必须关中断
  s_edgeTail = 0; s_edgeHead = 0;
  s_edgeTotal = 0; s_ovfTotal = 0;
  interrupts();
}

void PendulumApp::onButton() {
  if (!_measuring) {
    resetMeasure();
    _measuring = true;
    _idleEvSeen = 0;                 // 开始测量：清空待机摆动计数
  } else {
    _measuring = false;
    finalizeMeasure();                   // 生成历史记录 + 广播
  }
  _needDraw = true;
}

// ===================== 采集层体检（2026-10-03 重写） =====================
// 判读口诀（口径已随分层改造改变，2026-10-02 旧口诀作废）：
//   边沿=0            → 极性反 / 接线虚接 / 没遮断光路（硬件问题）
//   边沿成对持续增长  → 硬件与中断链路完好，问题在重建层或门限
//   队列溢出>0        → 边沿来得比主循环快，需调大 GATE_EDGE_RING（软件问题）
//   遮挡块>0 但计数不涨 → 停在待机没按开始，或被相对门限/离群判据连续剔除
// 关键设计：边沿数只增不减、永不判决，因此「硬件到底有没有看到摆动」
// 从此有一个不受任何软件判据影响的纯净答案。
void PendulumApp::gateDiagReport(const char* tag) {
  noInterrupts();
  uint32_t ev = s_edgeTotal, ovf = s_ovfTotal;
  interrupts();
  uint32_t blk = _nEdgePairs, spl = _mergedBlk, rej = _nRejected;
  uint32_t rec = _nRecovInt, out = _nOutlier, tmo = _nTimeout;
  uint32_t sdr = _nShortDrop, stk = _nStuck;
  double   typical = _blkTypicalUs / 1000.0;

  Serial.printf("[Pend][GATE:%s] 边沿=%u(亮/灭各半) 溢出=%u | 遮挡块=%u 分裂恢复=%u 剔除=%u 粘连=%u 整数倍恢复=%u 离群删除=%u 短落单=%u 超时=%u\n",
                tag, (unsigned)ev, (unsigned)ovf,
                (unsigned)blk, (unsigned)spl, (unsigned)rej, (unsigned)stk,
                (unsigned)rec, (unsigned)out, (unsigned)sdr, (unsigned)tmo);
  Serial.printf("[Pend][GATE:%s] 典型遮挡时长=%.2fms | 电气尖峰门限=%.0fus 片段合并=%.0f%%hRef 粘连门限=%.0f%%hTheo 中点门限=%.0f%%hTheo 离群=%.0f%%hRef\n",
                tag, typical, (double)GATE_BLINK_MIN_US,
                PEND_MERGE_REL * 100.0, PEND_STUCK_REL * 100.0,
                PEND_MINGAP_REL * 100.0, PEND_OUTLIER_REL * 100.0);
  // R7c：当前 GPIO 电平直读 + 宽容模式状态——判定「信号有没有到达/静止电平」
  // 只需一次 GATE，不再需要外部猜测。空槽静止时未遮挡电平即「正确极性基准」。
  Serial.printf("[Pend][GATE:%s] 当前电平=%s 宽容模式=%s\n", tag,
                digitalRead(PIN_OPTOGATE) ? "HIGH(未遮挡)" : "LOW(遮挡中)",
                _degraded ? "开(毛刺降级)" : "关(严格)");

  // —— 结论与可操作建议（这才是排查真正要的东西）——
  String verdict;
  if (ev == 0) {
    verdict = "完全没收到边沿：查DO极性、A0-GPIO4接线、挡光片是否真的遮断光路";
  } else if (ovf > 0) {
    verdict = "边沿队列溢出=" + String(ovf) + "：信号线振荡风暴，真边沿被挤掉——查电平转换/杜邦/MB102供电";
  } else if (stk > 0 && stk * 2 > blk + stk) {
    verdict = "粘连假块=" + String(stk) + "（超长" + String(typical, 0) + "ms遮挡）：信号贴阈值慢振荡，查电平转换模块/杜邦虚接/5V供电";
  } else if (blk == 0 && rej >= GATE_DEGRADE_REJ) {
    // R7c：剔除主导（≥8 个微秒级片段）时不再误导为「极性反」——极性反应产生
    // 秒级长片段（粘连计数），而这里全是毛刺。真实含义：挡光信号没有到达。
    verdict = "剔除主导(≥8个微秒级片段): 挡光仍未形成有效遮挡信号(宽容模式已默认开启)";
  } else if (blk == 0) {
    verdict = "有边沿但没配成完整遮挡块：极性可能反了（只出沿不进货）";
  } else if (spl > 0) {
    verdict = "已恢复粘合遮挡=" + String(spl) + "次（两次遮挡贴太近被并成一块，已分裂）";
  } else if (rej > 0 && rej * 2 > blk) {
    verdict = "过半遮挡被判电气尖峰剔除：挡光片太窄或摆动过快（看典型遮挡时长）";
  } else {
    verdict = "采集链路正常（边沿零丢弃）";
  }
  Serial.printf("[Pend][GATE:%s] 结论: %s\n", tag, verdict.c_str());

  if (Ble.connected()) {
    Ble.replyINFO("[GATE] 边沿=" + String((unsigned long)ev) + " 溢出=" + String((unsigned long)ovf) +
                  " 遮挡块=" + String((unsigned long)blk) + " 分裂恢复=" + String((unsigned long)spl) +
                  " 剔除=" + String((unsigned long)rej) +
                  " 粘连=" + String((unsigned long)stk) +
                  " 整数倍恢复=" + String((unsigned long)rec) +
                  " 离群删除=" + String((unsigned long)out) + " 短落单=" + String((unsigned long)sdr) +
                  " 超时=" + String((unsigned long)tmo));
    Ble.replyINFO("[GATE] 典型遮挡=" + String(typical, 2) + "ms 尖峰门限=" + String((unsigned long)GATE_BLINK_MIN_US) +
                  "us 片段合并=" + String(PEND_MERGE_REL * 100.0, 0) + "%hRef" +
                  " 粘连门限=" + String(PEND_STUCK_REL * 100.0, 0) + "%hTheo" +
                  " 中点门限=" + String(PEND_MINGAP_REL * 100.0, 0) + "%hTheo");
    Ble.replyINFO("[GATE] 结论: " + verdict);
  }
}

// ===================== 纠错报告（2026-10-03 新增） =====================
// 「g 算出来不对」在这套设计里不再是死胡同：所有被剔除的数据都留有账，
// 这条命令把「收了多少、剔了多少、恢复了多少、删了多少、纠正了几次」
// 一次列清，并解释 g 与标准值的偏差到底来自哪里。
void PendulumApp::fixDiagReport() {
  const double PI_D = 3.14159265358979323846;
  double hTheo = PI_D * sqrt(_pendLen / (_sG > 0 ? _sG : 9.794));   // 理论半周期
  double hMeas = (_nHalf > 0) ? (_sumHalf / _nHalf) : 0.0;          // 实测半周期均值
  double Tobs  = (_nPair > 0) ? (_sumPair / _nPair) : 2.0 * hMeas;  // 实际采用的周期
  double ratio = hTheo > 0 ? (Tobs / (2.0 * hTheo)) : 0.0;         // 实测/理论
  double err   = (_sG > 0) ? ((_Gavg - _sG) / _sG * 100.0) : 0.0;

  Serial.printf("[Pend][FIX] 样本: 计入=%d 参与平均=%d(成对%d组) 剔除=%u 粘连=%u 整数倍恢复=%u 离群删除=%u 短落单=%u 超时=%u 分裂恢复=%u\n",
                _nHalf, _nPair * 2, _nPair,
                (unsigned)_nRejected, (unsigned)_nStuck, (unsigned)_nRecovInt, (unsigned)_nOutlier,
                (unsigned)_nShortDrop, (unsigned)_nTimeout, (unsigned)_mergedBlk);
  Serial.printf("[Pend][FIX] 周期: 理论T=%.5fs 实测T=%.5fs 比值=%.4f 纠正系数=1/%.0f(已用%d次)\n",
                2.0 * hTheo, Tobs, ratio, _lastFixK, (unsigned)_fixApplied);
  Serial.printf("[Pend][FIX] 结果: Gavg=%.4f sG=%.4f 偏差=%+.2f%%\n", _Gavg, _sG, err);

  if (_fixApplied > 0) {
    Serial.printf("[Pend][FIX] 已自动纠正：%u 次按 1/%.0f 折算（检测到周期性丢样）\n",
                  (unsigned)_fixApplied, _lastFixK);
  } else if (_nOutlier > 0) {
    Serial.printf("[Pend][FIX] 未触发倍率纠正：已删除%u个离群点，剩余数据自洽\n", (unsigned)_nOutlier);
  } else {
    Serial.printf("[Pend][FIX] 未触发倍率纠正：数据自洽，无需干预\n");
  }

  if (Ble.connected()) {
    Ble.replyINFO("[FIX] 样本 计入=" + String(_nHalf) + " 参与=" + String(_nPair * 2) +
                  " 剔除=" + String((unsigned long)_nRejected) +
                  " 整数倍恢复=" + String((unsigned long)_nRecovInt) +
                  " 离群删除=" + String((unsigned long)_nOutlier) +
                  " 碎片合并=" + String((unsigned long)_mergedBlk) +
                  " 超时=" + String((unsigned long)_nTimeout));
    Ble.replyINFO("[FIX] 理论T=" + String(2.0 * hTheo, 5) + "s 实测T=" + String(Tobs, 5) +
                  "s 比值=" + String(ratio, 4) + " 纠正=1/" + String(_lastFixK, 0) +
                  " (已用" + String((unsigned long)_fixApplied) + "次)");
    Ble.replyINFO("[FIX] Gavg=" + String(_Gavg, 4) + " sG=" + String(_sG, 4) +
                  " 偏差=" + String(err, 2) + "%" +
                  (_fixApplied ? " [已自动纠正]" : " [无需纠正]"));
  }
}

// ===================== 原始边沿转储（2026-10-03 新增） =====================
// 「每一次亮起或熄灭都被记录」这句话需要能被验证，而不是靠承诺。
// EDGES 把队列里的原始边沿逐条打出（时刻 + 电平），
// 现场就能核对：数一数 ↓ 出现几次，就是摆球真正经过光路几次。
void PendulumApp::edgesDump() {
  noInterrupts();
  int n = 0;
  for (uint8_t i = s_edgeTail; i != s_edgeHead; i = (uint8_t)((i + 1) & (EDGE_RING - 1))) n++;
  int shown = 0;
  String body = "";
  for (uint8_t i = s_edgeTail; i != s_edgeHead && shown < 32;
       i = (uint8_t)((i + 1) & (EDGE_RING - 1)), shown++) {
    uint32_t t = s_edgeUs[i];
    uint8_t  lv = s_edgeLv[i];
    if (shown) body += " ";
    body += String((unsigned long)(t / 1000UL)) + "ms" + (lv ? "\u2191" : "\u2193");
  }
  uint32_t ev = s_edgeTotal, ovf = s_ovfTotal;
  interrupts();

  Serial.printf("[Pend][EDGES] 队列内=%d 累计=%u 溢出=%u\n", n, (unsigned)ev, (unsigned)ovf);
  Serial.printf("[Pend][EDGES] %s\n", body.c_str());
  if (Ble.connected()) {
    Ble.replyINFO("[EDGES] 队列内=" + String(n) + " 累计=" + String((unsigned long)ev) +
                  " 溢出=" + String((unsigned long)ovf));
    Ble.replyINFO("[EDGES] " + body);
  }
}

// ===================== 测量结算 =====================

void PendulumApp::finalizeMeasure() {
  if (_nHalf <= 0) {
    // 无数据不记录，但必须给用户可见反馈——否则按一下停止后屏幕只回到待机，
    // 用户无从得知"这次摆动根本没被记录"，会误以为数据丢了或已存盘
    Display.showNotification("(-_-) 无有效数据，未记录", 4000);
    Serial.println("[Pend] measure stopped with no samples, not recorded");
    return;
  }

  // 波形流：测量停止时自动关闭
  if (_streaming) {
    _streaming = false;
    Ble.streamEnd();
  }

  double err  = (_sG > 0) ? ((_Gavg - _sG) / _sG * 100.0) : 0.0;

  // —— 落单样本处理（2026-10-02 配对纠正）——
  // 若最后一次快慢没有配成对（停止时手上还挂着一个），该样本已被排除在 Gavg 之外。
  // 这里如实记账并在串口/屏幕说明，避免"屏幕 n 与实际参与平均的样本数不一致"。
  if (_hasPend) {
    _nDropped = 1;
    _hasPend = false;
    _pendHalf = 0;
  }
  int nUsed = _nPair * 2;                // 实际参与 Gavg 的半周期样本数
  double Tavg = (nUsed > 0) ? (_sumPair / _nPair) : (2.0 * _sumHalf / _nHalf);

  // —— R7-06（2026-10-06）：数据来源自检 ——
  // 有样本但边沿总数为 0 = 数据不是来自光电门 ISR（历史记录 27 组出现过
  // 边沿=0/剔除=12448 的矛盾组合，成因未定，先留自检痕迹便于事后排查）。
  noInterrupts();
  uint32_t edgeTotalAtFinalize = s_edgeTotal;
  interrupts();
  if (edgeTotalAtFinalize == 0 && nUsed > 0) {
    Serial.println("[Pend][警告] 结算时边沿总数=0 但有样本：数据非来自 ISR 路径，请检查！");
  }

  // —— 周期核对（判读依据已换成实测/理论比值）——
  // 现象回顾：用户实测 g≈1~2。数学上 Tavg 偏大只能来自「中点事件丢失」：
  // 每个事件本应间隔半个周期 h，若每隔一个才被采纳，采纳间隔=T，配对求和再 ×2
  // → Tavg=2T → g=真实/4≈2.45（正好落在观测的 1~2 区间）。
  // 2026-10-03 起两种丢样（偶发漏沿 / 系统性 m 倍）都在 onTick 内被自动纠正了
  // （整数倍恢复 / 系统性倍率纠正），所以这里的核对是最后一道复核：
  // 比值仍显著偏离 1，说明问题不在丢样，而在摆长填错。
  double hTheo = 3.14159265358979323846 * sqrt(_pendLen / (_sG > 0 ? _sG : 9.794));
  double ratio = (hTheo > 0) ? (Tavg / (2.0 * hTheo)) : 0;

  Serial.printf("[Pend] 结算: n=%d 参与=%d(成对%d组) Tavg=%.5f Gavg=%.4f 交替=%s%s\n",
                _nHalf, nUsed, _nPair, Tavg, _Gavg,
                _altDetected ? "有" : "无",
                _nDropped ? " 落单1个已排除" : "");
  gateDiagReport("stop");   // 采集层体检：边沿/块/剔除/恢复（2026-10-03）
  fixDiagReport();          // 纠错账目：恢复/删除/倍率纠正（2026-10-03）
  Serial.printf("[Pend] 周期核对: 理论T=%.4fs 实测T=%.4fs 比值=%.3f -> %s\n",
                2.0 * hTheo, Tavg, ratio,
                (ratio > 1.15 || ratio < 0.87) ? "异常！疑似摆长单位错或仍有丢样"
                                              : "正常");
  if (ratio > 1.15 || ratio < 0.87) {
    // 比值不是整数倍附近 → 多半是摆长填错（cm 当 m 用会得到 100 倍关系）
    Display.showNotification("周期异常,请核对摆长", 4000);
  }

  // 历史记录 CSV：序号,YYYY-MM-DD HH:MM,L,N,Tavg,Gavg,err%,[新增]边沿,剔除,恢复,离群,纠正系数
  // 后 5 列是 2026-10-03 追加的纠错账目，ble_cli 只读 row[2](L) 与 row[4](T)，
  // 追加在尾部不影响既有解析（已核对 tools/ble_cli.py）。
  // R7（2026-10-06）再追加 2 列：队列溢出、短样本落单删除（双遮挡救回失败的残留）。
  HistoryStore hist("app2", HIST_CAP);
  String line = String(hist.count() + 1) + "," + Time.csvDateTime() + "," +
                String(_pendLen, 3) + "," + String(nUsed) + "," +
                String(Tavg, 3) + "," + String(_Gavg, 3) + "," + String(err, 2) + "%," +
                String((unsigned long)edgeTotalAtFinalize) + "," +
                String((unsigned long)_nRejected) + "," +
                String((unsigned long)(_nRecovInt + _mergedBlk)) + "," +
                String((unsigned long)_nOutlier) + "," +
                String(_lastFixK, 0) + "," +
                String((unsigned long)s_ovfTotal) + "," +
                String((unsigned long)_nShortDrop);
  hist.append(line);

  // 广播测量完成（程序1 庆祝表情联动）
  Event e; e.type = EventType::EVT_MEASURE_DONE; e.value = (float)_Gavg;
  EventBus::broadcast(e);
  Serial.println("[Pend] measure done: " + line);
}

// ===================== 当地标准 g（分册02第5节） =====================

void PendulumApp::computeSg() {
  // sG = 9.780327×(1 + 0.0053024·sin²φ − 0.0000058·sin²2φ) − 3.086e-6×h
  String latS;
  Store.getString("sys.loc_lat", latS);
  double lat = latS.toDouble();
  if (lat <= 0) lat = 40.77;             // 默认张家口一中纬度
  double alt = Store.getDouble("app2.alt_m", 0.0);   // 海拔默认 0，BLE 会话 "alt <m>" 可改
  const double PI_D = 3.14159265358979323846;
  double phi = lat * PI_D / 180.0;
  double s = 9.780327 * (1.0 + 0.0053024 * sin(phi) * sin(phi)
                              - 0.0000058 * sin(2.0 * phi) * sin(2.0 * phi))
                - 3.086e-6 * alt;
  _sG = s; _sgSrc = "CALC"; _sgTs = Time.epoch();
  Store.setDouble("app2.sg_cache", s);
  Store.setString("app2.sg_src", "CALC");
  Store.setInt("app2.sg_ts", (int32_t)_sgTs);
}

void PendulumApp::onEvent(const Event& evt) {
  // 联网恢复 / 坐标可能变更（程序3 改坐标后拉取）→ 重算 sG
  if (evt.type == EventType::EVT_NET_GOT || evt.type == EventType::EVT_WX_UPDATE_OK) {
    computeSg();
    _needDraw = true;
  }
  // BLE 断连 → 流自动停止（重连后需手动重新开启）
  if (evt.type == EventType::EVT_BLE_DISC && _streaming) {
    _streaming = false;
    Serial.println("[Pend] BLE lost, stream stopped");
  }
}

// ===================== 重建层：边沿 → 遮挡块 → 中点 → 纠错 =====================
// 本函数是 2026-10-03 改造的全部核心。设计原则：
//   1) ISR 只负责「记」，本层负责「算」。所有判决都在这里，可被推翻重算。
//   2) 门限一律以 hRef（半周期标尺）为单位，不用绝对微秒数——绝对门限在
//      L 可变（0.1~5m ⇒ 半周期 0.63~4.5s）的实验里必然要么太松要么太紧。
//   3) 每一个被剔除/恢复/删除的样本都计数留账，FIX 命令一次列清。
//   4) 宁可先记下再判断，也不先判断再丢弃：漏记不可逆，误记可以再删。

void PendulumApp::onTick(float dt) {
  (void)dt;
  uint32_t now = millis();
  const double PI_D = 3.14159265358979323846;

  // ================= A. 模拟数据注入（无光电门自测，R5 新增） =================
  // 注入的是「边沿」而不是「样本」——刻意与真实硬件走完全相同的路径，
  // 因此 SIM 验证的是真实的重建+纠错代码，而不是绕过它。
  if (_simOn && _measuring) {
    uint32_t nowUs = micros();
    int guard = 0;
    // 一次模拟遮挡 = 一对边沿：LOW 后 simBlkUs 转 HIGH。
    // 遮挡时长取半周期的 1/20（L=1m 时约 50ms），确保不触发「合并块分裂」。
    uint32_t simBlkUs = _simHalfUs / 20; if (simBlkUs < 2000) simBlkUs = 2000;
    while ((int32_t)(nowUs - _simLastUs) >= 0 && guard < 16) {
      noInterrupts();
      uint8_t nx = (uint8_t)((s_edgeHead + 1) & (EDGE_RING - 1));
      if (nx == s_edgeTail) s_edgeTail = (uint8_t)((s_edgeTail + 1) & (EDGE_RING - 1));
      s_edgeUs[s_edgeHead] = _simLastUs;            // 进遮挡
      s_edgeLv[s_edgeHead] = 0;
      s_edgeHead = nx;
      uint8_t nx2 = (uint8_t)((s_edgeHead + 1) & (EDGE_RING - 1));
      if (nx2 == s_edgeTail) s_edgeTail = (uint8_t)((s_edgeTail + 1) & (EDGE_RING - 1));
      s_edgeUs[s_edgeHead] = _simLastUs + simBlkUs;  // 出遮挡
      s_edgeLv[s_edgeHead] = 1;
      s_edgeHead = nx2;
      s_edgeTotal += 2;
      interrupts();
      _simLastUs += _simHalfUs;
      guard++;
    }
    if (guard >= 16) _simLastUs = nowUs;  // onTick 卡顿太久则重新对齐，避免追样失控
  }

  // ================= B. 半周期标尺 hRef =================
  // 优先实测（滑动均值，随纠错实时更新），样本不足时退回理论值。
  // 理论值 h=π√(L/g) 来自用户输入的摆长与当地标准 g，能给早期样本一个
  // 正确判据，避免「开头几个样本因为没有标尺而被误删」。
  double hTheo = PI_D * sqrt(_pendLen / (_sG > 0 ? _sG : 9.794));
  double hRef  = (_nHalf >= 3) ? (_sumHalf / _nHalf) : hTheo;
  // 实测与理论严重不符（>40%）时，继续用实测标尺会自我强化错误
  // （摆长填错 → 标尺偏 → 判据偏 → 更多样本被误删）。此时改用理论标尺。
  bool lenSuspicious = (hTheo > 0) && (hRef > 0) && (fabs(hRef / hTheo - 1.0) > 0.40);
  if (lenSuspicious) hRef = hTheo;
  uint32_t hRefUs = (hRef > 0) ? (uint32_t)(hRef * 1e6) : 0;
  uint32_t hTheoUs = (hTheo > 0) ? (uint32_t)(hTheo * 1e6) : 0;   // R7-02：理论标尺（恒可靠）

  // ================= C. 排空边沿队列，重建遮挡块与中点 =================
  // 单条临界区取一条边沿后立刻开中断，避免长时间关中断影响计时精度。
  bool got = false;
  for (;;) {
    uint32_t t; uint8_t lv;
    noInterrupts();
    bool empty = (s_edgeTail == s_edgeHead);
    if (empty) { t = 0; lv = 0; }
    else {
      t  = s_edgeUs[s_edgeTail];
      lv = s_edgeLv[s_edgeTail];
      s_edgeTail = (uint8_t)((s_edgeTail + 1) & (EDGE_RING - 1));
    }
    interrupts();
    if (empty) break;

    // —— 边沿 → 遮挡片段（R7b：半途片暂存 + 毛刺截断回滚）——
    // 正常路径：↓ 开块、↑ 闭合后交给 gateProcessSeg（剔除/合并/发布/统计一条龙）。
    // LOW 期间又见 LOW（半途片）：不再立即结算 —— 触发它的 LOW 可能是一个
    // 微秒级毛刺（粘连段内振荡），立即结算会把粘连块肢解成 4.5ms 级假片混进
    // 中点流（仿真 S12 实证 +5.2% 偏差）。半途片先进暂存区，延迟一个边沿裁决：
    //   若下一个出沿距暂存末端 <200µs 且暂存片非粘连 → 截断者是毛刺，
    //   作废暂存片并将会话恢复到被截断 LOW 的起点（毛刺无权打断遮挡）；
    //   否则暂存片确认有效，连同当前块一起正常处理。
    if (lv == 0) {                       // ↓ 进遮挡
      if (_blkOpen) {
        // 上一个暂存片被新 LOW 确认（真实遮挡重叠）：先处理再开新暂存
        if (_psValid) {
          if (gateProcessSeg(_psTIn, _psEnd, true, hTheo, hRef, hRefUs, hTheoUs, now))
            got = true;
          _psValid = false;
        }
        _psTIn = _blkTIn; _psEnd = t; _psValid = true;
        _blkTIn = t;
        continue;
      }
      _blkOpen = true;
      _blkTIn  = t;
      continue;
    }
    // ↑ 出遮挡
    if (!_blkOpen) continue;             // 只有出沿没有入沿：极性/接线问题，不构造数据
    if (_psValid) {
      // R7c：截断判定门限跟随宽容模式（200µs→20µs），第二条件（粘连上限）不变
      uint32_t blinkMinUs = _degraded ? (uint32_t)GATE_BLINK_MIN_FB_US
                                      : (uint32_t)GATE_BLINK_MIN_US;
      bool glitchTrunc = ((uint32_t)(t - _psEnd) < blinkMinUs) &&
                         ((uint32_t)(_psEnd - _psTIn) <
                          (uint32_t)((double)hTheoUs * PEND_STUCK_REL));
      if (glitchTrunc) {
        // 毛刺截断确认：作废暂存片，会话恢复到粘连/原 LOW 起点（遮挡仍在继续）
        _blkTIn = _psTIn;
        _psValid = false;
        continue;
      }
      // 暂存片确认有效：先处理它，再闭合当前块
      if (gateProcessSeg(_psTIn, _psEnd, true, hTheo, hRef, hRefUs, hTheoUs, now))
        got = true;
      _psValid = false;
    }
    {
      uint32_t sTIn = _blkTIn;
      _blkOpen = false;
      if (gateProcessSeg(sTIn, t, false, hTheo, hRef, hRefUs, hTheoUs, now))
        got = true;
    }
    continue;
  }
  if (got) _needDraw = true;

  // （R7c 修订：宽容模式默认开启，原「30s 零产出自动降级」触发块已删除——
  //   用户实验不允许先等 30s。见 fw_config.h 与 app2_pendulum.h 注释。）

  // —— 摆长疑似填错：每 2s 提示一次（不阻断测量，但必须明确告诉用户）——
  if (lenSuspicious && _measuring && (int32_t)(now - _lastDiagMs) >= 2000) {
    _lastDiagMs = now;
    double hm = _nHalf ? (_sumHalf / _nHalf) : hTheo;
    Serial.printf("[Pend][FIX] 实测半周期=%.3fs 理论=%.3fs(L=%.3fm) 偏差%+.0f%%："
                  "请核对摆长单位（cm 当 m 用最常见）\n", hm, hTheo, _pendLen,
                  (hTheo > 0 ? (hm / hTheo - 1.0) * 100.0 : 0.0));
  }

  // —— 待机防呆（2026-10-03 改用重建出的块数）——
  // 依据重建计数而非队列是否非空：若边沿还没配成块，循环体一次都不进。
  if (!_measuring) {
    if (_nEdgePairs > 0) {
      _idleHintMs = (int32_t)(millis() + 3000);   // 提示持续 3s
      _needDraw = true;
    }
    if (got) _idleEvSeen++;   // 待机摆动：样本丢弃（设计行为），记一笔供屏幕提示
  }

  // —— 测量中每 2s 打印一行采集体检（串口静默排查用）——
  if (_measuring && (int32_t)(now - _lastDiagMs) >= 2000) {
    _lastDiagMs = now;
    gateDiagReport("live");
  }

  // BLE 历史导出：分块发送（每 tick ≤8 行，不阻塞）
  if (_exporting) {
    if (!Ble.connected()) { /* 断连暂停，重连后继续 */ }
    else {
      HistoryStore hist("app2", HIST_CAP);
      hist.readRange(_exportIdx, 8, [this](int, const String& line) -> bool {
        if (line.length()) Ble.exportData(line);
        _exportIdx++;                      // 无论是否空行都推进，保证一定能收敛到 END
        return true;
      });
      if (_exportIdx >= _exportCnt) {
        Ble.exportEnd();
        _exporting = false;
        Ble.replyOK("导出完成");
      }
    }
  }

  // 刷新：有新数据立即刷；否则 1s 心跳（计时/状态角标）
  if (_needDraw || now - _lastDrawMs >= 1000) {
    _lastDrawMs = now;
    _needDraw = false;
    draw();
  }
}

// ===================== 片段处理：剔除 → 合并 → 发布（R7b 抽取） =====================
// 一个片段的全部判决：BLINK 尖峰剔除、STUCK 粘连剔除、同源合并（含 R7b 链上限）、
// pending 发布（R7-07）。发布旧 pending 时立即调 gateProcessReplay 消费中点。
// 返回 true = 本段发布产生了有效样本（onTick 据此点亮 _needDraw）。
bool PendulumApp::gateProcessSeg(uint32_t sTIn, uint32_t sEnd, bool partial,
                                 double hTheo, double hRef, uint32_t hRefUs, uint32_t hTheoUs,
                                 uint32_t now) {
  uint32_t dur = sEnd - sTIn;
  // R7c：尖峰门限跟随宽容模式（200µs→20µs，旧版等效）
  uint32_t blinkMin = _degraded ? (uint32_t)GATE_BLINK_MIN_FB_US
                                : (uint32_t)GATE_BLINK_MIN_US;
  if (dur < blinkMin) {
    // 200µs 以下的硬件尖峰/振荡碎片段（R7b 由 20µs 上调）。采集路径上保留的
    // 剔除（含义是「这不是一次摆动」）。真实遮挡 ≥4.7ms，对 200µs 有 23 倍裕度；
    // 电平转换慢边沿振荡的碎片级翻转（实测 75s 灌 5536 边沿）绝大多数落在此
    // 门限以下，就地掐掉，不再进入合并/中点流。
    _nRejected++;
    return false;
  }
  // —— 粘连/振荡假块剔除（R7b，2026-10-06）——
  // 实测振荡环境出现典型 740ms 的「遮挡块」：信号贴着阈值慢振荡，把 LOW 段
  // 拉长数十倍（真实纸片遮挡仅 15~45ms）。判据：完整片段时长 > 0.50×hTheo
  // （L=0.428m 时 328ms）必假 —— 物理上遮挡时长 = 遮挡宽÷速度，最宽遮挡物+
  // 最慢释放的极端组合也只有 ~154ms。半途片（partial）同判。
  // 账目 _nStuck 供 GATE 体检直接看出「信号环境病态」。
  // R7c：宽容模式停用（旧版无此剔除；降级环境需要放行长片段出数）。
  if (!_degraded && hTheoUs && dur > (uint32_t)((double)hTheoUs * PEND_STUCK_REL)) {
    _nStuck++;
    return false;
  }
  uint32_t segMid = sTIn + dur / 2;

  // —— 同源片段合并（R7-01 门限 0.45×hRef + R7-07 pending 发布制）——
  // 真实的漏记只有一种：传感器漏掉整个跳变沿 → 间隔变 2h/3h，由统一流的
  // 「整数倍恢复」处理，与本段无关。而 LOW 段裂成几片（挡光片边缘抖动、
  // 电平转换 RC 碎边沿、双遮挡贴近）是「同一次过零的碎片」，应合并成 1 个
  // 中点，否则每个真实半周期产出 2 个假中点（g 虚增 4 倍，实测定向复现）。
  // R7-07 pending 发布制：闭合片段先进 pending，下一片段先判同源合并，
  // 不同源才把 pending 发布进统一流 —— 统一流只收「确定的」最终中点，零残影
  // （旧实现合并修正中点二次入流，_lastMidUs 被推进 Δ/2，仿真 S3 复现 G=13.6）。
  bool merged = false;
  if (!partial && _pendSegValid && hRefUs > 0 && sTIn > _pendSegEndUs) {
    uint32_t gapUs = sTIn - _pendSegEndUs;
    // gapUs==0（出沿与新入沿同刻）是半途片结算假象，不是同源碎片，必须发布分开
    if (gapUs > 0 && gapUs < (uint32_t)((double)hRefUs * PEND_MERGE_REL)) {
      uint64_t wsum = (uint64_t)_pendSegDur + dur;
      // —— 合并链总时长上限（R7b，2026-10-06）——
      // 粘连段内的毛刺会把 LOW 段肢解成一串 4.5ms 级「正常片」，gap 全落进
      // 合并窗口 → 连锁合并让 pending 无限增长、中点漂移到粘连区间中部，
      // 以「合法间隔」发布污染统计（仿真 S12 复现 G=+5.2%）。物理约束：正常
      // 合并链 = 双遮挡碎片，总时长 = 一次遮挡时长 ≤45ms；链总时长 >0.50×hTheo
      // 必然是粘连 → 整链作废留账。
      if (!_degraded && hTheoUs && wsum > (uint64_t)((double)hTheoUs * PEND_STUCK_REL)) {
        _nStuck++;
        _pendSegValid = false;
        _pendSegDur = 0; _pendSegEndUs = 0; _pendSegMidUs = 0;
        return false;
      }
      // 中点取两片按时长加权（加权平均 = 整段几何中心，相对真实过零零偏差）
      uint64_t msum = (uint64_t)_pendSegMidUs * _pendSegDur + (uint64_t)segMid * dur;
      _pendSegMidUs = (uint32_t)(msum / wsum);
      _pendSegDur   = (uint32_t)wsum;
      _pendSegEndUs = sEnd;
      _mergedBlk++;
      merged = true;
      // 合并不新增计数：这次过零在上一片已记过账
    }
  }
  if (!merged) {
    // 与 pending 不同源：pending 的合并窗口关闭，发布它进统一流
    if (_pendSegValid) {
      _replayMid = _pendSegMidUs;
      _hasReplay = true;
    }
    // 当前片段成为新 pending，等下一个片段判合并后再发布
    _pendSegValid = true;
    _pendSegEndUs = sEnd;
    _pendSegDur   = dur;
    _pendSegMidUs = segMid;
    // 典型遮挡时长：只采信干净的完整片（指数平滑 3:1）
    if (!partial) {
      if (_blkTypicalUs == 0) _blkTypicalUs = dur;
      else _blkTypicalUs = (_blkTypicalUs * 3 + dur) / 4;
    }
    _nEdgePairs++;
  }
  if (_hasReplay) return gateProcessReplay(hTheo, hRef, hRefUs, hTheoUs, now);
  return false;
}

// ===================== 统一统计流：所有中点都从这里走一遍（R7b 抽取） =====================
// 单一入口是本层正确性的关键：pending 发布的最终中点（含同源合并后的）与正常
// 中点走完全相同的判决与累计逻辑。R7-07 起本流只接收「确定的」过零中点。
// 返回 true = 该中点被采纳计入了统计（供 onTick 点亮 _needDraw）。
bool PendulumApp::gateProcessReplay(double hTheo, double hRef, uint32_t hRefUs, uint32_t hTheoUs,
                                    uint32_t now) {
  const double PI_D = 3.14159265358979323846;
  uint32_t mid = _replayMid;
  _hasReplay = false;
  if (!_midValid) { _lastMidUs = mid; _midValid = true; return false; }
  uint32_t dm = mid - _lastMidUs;
  _lastMidUs = mid;
  if (dm > GATE_TIMEOUT_US) { _nTimeout++; _midValid = false; return false; }  // 停摆/重启

  // —— R7-05 待机门卫（2026-10-06）——
  // 待机只记摆动次数，不进统计（开始测量时 resetMeasure 清零，不影响正式数据）。
  if (!_measuring) { _idleEvSeen++; return false; }

  // —— R7-03 半样本配对合并（2026-10-06）：双遮挡假中点救回 ——
  // 双遮挡（摆球+挡光片）中点间隔呈 Δ 与 h−Δ 两簇交替（Δ≤0.5h）。两簇样本
  // 都 < 0.80×hTheo 且之和 ≈ hTheo → 合并成 1 个真样本（时间守恒，g 无偏）；
  // 落单短样本 → 删除留账。必须放在相对门限之前，否则短簇先被门限吃掉，
  // 只剩 h−Δ 簇以 ~25% 周期偏差污染 g（对应 G=17~25 一类杂乱值）。
  // R7c：宽容模式保留本判据（统计层是数据质量守门员——片段层放宽后毛刺
  // 中点全靠这里与离群删除清理，真摆样本不受影响）。
  double halfT0 = dm / 1e6;
  if (hTheo > 0 && halfT0 < PEND_SHORT_MAX_REL * hTheo) {
    if (_hasShort) {
      double sum2 = _pendShort + halfT0;
      if (fabs(sum2 - hTheo) < PEND_SHORT_PAIR_TOL * hTheo) {
        dm = (uint32_t)(sum2 * 1e6 + 0.5);   // 合并：时间守恒，间隔 = 两簇之和
        _pendShort = 0; _hasShort = false;
        // dm 现在 ≈ hTheo，向下走正常采纳路径
      } else {
        _nShortDrop++;                        // 前一短样本落单：删除
        _pendShort = halfT0;                  // 当前短样本继续待配
        return false;
      }
    } else {
      _pendShort = halfT0; _hasShort = true;  // 首个短样本：挂起待配
      return false;
    }
  } else if (_hasShort) {
    _nShortDrop++; _pendShort = 0; _hasShort = false;  // 短样本被长间隔打断：落单删除
  }

  // —— 相对门限：远小于半周期者判电气毛刺/抖动 ——
  // R7-02：标尺用 hTheo（理论值，恒可靠），不用可被假样本污染的 hRef。
  // R7c：宽容模式保留（理由同上——统计层判据不参与「零产出」，只做质检）。
  if (hTheoUs && dm < (uint32_t)((double)hTheoUs * PEND_MINGAP_REL)) { _nRejected++; return false; }
  if (dm < GATE_MINGAP_US) { _nRejected++; return false; }   // 绝对兜底（2ms）

  // —— 整数倍恢复：d ≈ m×hRef 说明中间漏了 m−1 次 ——
  // 时间总量守恒（sumHalf 加的是同一段真实时间），g 不受影响，恢复的只是
  // n 计数与波形里的真实摆动次数。
  int mi = 1;
  if (hRefUs) {
    double rr = (double)dm / (double)hRefUs;
    int cand = (int)(rr + 0.5);
    if (cand >= 2 && cand <= PEND_FIX_MAXK * 2 && fabs(rr - cand) / cand < PEND_INT_TOL)
      mi = cand;
  }

  double halfT = dm / 1e6;
  if (mi >= 2) {
    _nRecovInt += (uint32_t)(mi - 1);
    _sumHalf += halfT;        // 时间守恒
    _nHalf   += mi;           // 计数恢复：该记几次就记几次
    _lastT = 2.0 * halfT / (double)mi;
    // m 个半周期 = m/2 个完整周期（m 偶）。m 奇会改变配对奇偶性，
    // 故显式重置待配对状态，让后续样本重新配对。
    if ((mi & 1) == 0) { _sumPair += halfT; _nPair += mi / 2; _lastPairT = halfT / (double)(mi / 2); }
    _hasPend = false; _pendHalf = 0;
  } else {
    // —— 离群删除：明显不符合规律的数据（R7-04：始终启用，无免检窗口）——
    // 放在并入之前判决：删的是「单个样本」，不会在配对序列里留空洞。
    // 25% 宽容度足以放过 ±8% 的快慢交替。连续删除达 15 个时提醒一次。
    // R7c：宽容模式保留且更关键——片段层放宽后毛刺假中点（间隔远偏离 hRef）
    // 全靠这里删除，真摆样本（≈hRef）畅通，出数与精度两全。
    if (hRefUs) {
      double dev = fabs(halfT - hRef) / hRef;
      if (dev > PEND_OUTLIER_REL) {
        _nOutlier++;
        if (!_outlierWarned && _nOutlier >= 15) {
          _outlierWarned = true;
          Display.showNotification("连续剔除异常样本,请查挡光片/摆长", 3500);
          Serial.println("[Pend][FIX] 已连续剔除 15+ 个异常样本（疑似双遮挡或摆长错误）");
        }
        return false;   // 不并入：删除
      }
    }
    // —— 快慢交替检测（2026-10-02）——
    // 光电门偏离最低点时，相邻半周期一短一长且幅度对称。阈值 1.5%：真噪声
    // 远小于此，而 3mm 偏移就产生 7~8% 差异 → 判定裕度充足。
    if (_hasPend && _pendHalf > 0) {
      double ref = (_pendHalf + halfT) * 0.5;
      double rel = (halfT - _pendHalf) / ref;
      if (fabs(rel) > 0.015) {
        if (!_altDetected) {
          _altDetected = true;
          _altAmp = fabs(rel) * 100.0;
          Serial.printf("[Pend] 检出快慢交替：相邻半周期差 %.1f%%（光电门可能偏离最低点）\n",
                        rel * 100.0);
          Display.showNotification("检出快慢交替,已按对平均", 3000);
        } else {
          double a = fabs(rel) * 100.0;
          if (a > _altAmp) _altAmp = a;   // 记录最大交替幅度
        }
        _altCnt++;
      }
    }
    // —— 配对结算：Gavg 只用配对结果，天然免疫奇偶与光电门偏移 ——
    _sumHalf += halfT; _nHalf++;
    _lastT = 2.0 * halfT;
    if (!_hasPend) { _pendHalf = halfT; _hasPend = true; }
    else {
      double pairT = _pendHalf + halfT;
      _sumPair += pairT; _nPair++; _lastPairT = pairT;
      _pendHalf = 0; _hasPend = false;
    }
  }

  double Tavg = (_nPair > 0) ? (_sumPair / _nPair) : (2.0 * _sumHalf / _nHalf);
  double Tnow = (_nPair > 0) ? _lastPairT : _lastT;
  _lastG = 4.0 * PI_D * PI_D * _pendLen / (Tnow * Tnow);
  _Gavg  = 4.0 * PI_D * PI_D * _pendLen / (Tavg * Tavg);

  // —— 系统性倍率纠正（只缩小方向：丢样只会让周期变大，方向无歧义）——
  // 实测 T 落在理论 T 的 m 倍（m=2..4）附近 → 判定周期性丢样，按 1/m 折算。
  // 样本 ≥4 对才启用；反方向不自动改，只提示查摆长。
  if (_nPair >= 4 && hTheo > 0) {
    double Tth = 2.0 * hTheo;
    double Tcur = _sumPair / _nPair;
    double r2 = Tcur / Tth;
    if (r2 > 1.0) {
      int m2 = (int)(r2 + 0.5);
      if (m2 >= 2 && m2 <= PEND_FIX_MAXK && fabs(r2 - m2) / m2 < PEND_FIX_TOL) {
        double Tcorr = Tcur / (double)m2;
        double Gcorr = 4.0 * PI_D * PI_D * _pendLen / (Tcorr * Tcorr);
        if (_lastFixK != (double)m2) {
          _fixApplied++;
          Serial.printf("[Pend][FIX] 自动纠正：实测T=%.5fs=理论T×%d（周期性丢样）"
                        "→ 按1/%d折算，Gavg %.4f → %.4f\n",
                        Tcur, m2, m2,
                        4.0 * PI_D * PI_D * _pendLen / (Tcur * Tcur), Gcorr);
          Display.showNotification("已自动纠正丢样 x" + String(m2), 3500);
        }
        _lastFixK = (double)m2;
        _Gavg = Gcorr;
      }
    }
  }

  // —— 实时波形流推送（phyphox 兼容 #STREAM 协议）——
  if (_streaming && Ble.connected() && now - _lastStreamMs >= 50) {
    _streamSeq++;
    _lastStreamMs = now;
    // 格式：#STREAM <seq>,<dt_us>,<T>,<g_inst>,<g_avg>,<n>,<L>
    String csv = String(_streamSeq) + "," +
                  String(dm) + "," +
                  String(_lastT, 6) + "," +
                  String(_lastG, 4) + "," +
                  String(_Gavg, 4) + "," +
                  String(_nHalf) + "," +
                  String(_pendLen, 3);
    Ble.streamData(csv);
  }
  return true;
}


// ===================== 绘制（分册02第4节四行布局，字段名不动） =====================

void PendulumApp::draw() {
  if (!Display.present()) return;
  Display.clearBuffer();

  Display.setFont(DisplayService::FONT_ROW);
  Display.drawStr(0, 10, ("g=" + String(_lastG, 3) + " m/s2").c_str());
  // n= 半周期总数（已含整数倍恢复补回的次数），口径与波形/历史一致
  Display.drawStr(0, 21, ("n=" + String(_nHalf) + " t=" + String(_lastT, 3) + "s").c_str());
  Display.drawStr(0, 32, ("L=" + String(_pendLen, 3) + " sG=" + String(_sG, 3)).c_str());
  // N= 「参与平均的样本数」（成对口径，恒为偶数）——与 Gavg 的实际口径一致，
  // 末尾 * 表示尚有 1 个落单样本未配对（按了停止键才结算）
  String nTag = _hasPend ? "*" : "";
  Display.drawStr(0, 43, ("N=" + String(_nPair * 2) + nTag + " G=" + String(_Gavg, 3)).c_str());

  // 角标：err + 状态 + sG 数据年龄（仅 sG 来自缓存时，分册02第4节）
  double err = (_sG > 0) ? ((_Gavg - _sG) / _sG * 100.0) : 0.0;
  Display.setFont(DisplayService::FONT_TINY);
  // 检出快慢交替时在行尾追加标记（ALT=幅度%），行内不改字段位置
  String altTag = _altDetected ? (" ALT" + String(_altAmp, 0) + "%") : "";
  // 2026-10-03：自动纠正生效时挂 FIXxN 角标，让用户知道 G 是折算过的。
  // 不标注等于让一个被软件改过的数字冒充实测值，这是必须避免的误导。
  String fixTag = (_fixApplied > 0) ? (" FIXx" + String(_lastFixK, 0)) : "";
  Display.drawStr(0, 63, ("err=" + String(err, 2) + "%" + fixTag + altTag).c_str());

  Display.setFont(DisplayService::FONT_CN12);
  Display.rightUtf8(63, _measuring ? "[测量中]" : "[待机]");

  // 防呆提示（2026-10-02/03）：待机时若已重建出摆动，明确告诉用户「需要先按开始」，
  // 否则摆了很多次却看不到 n 增长，极易误判为硬件故障。
  // 2026-10-03 起判据全部改用重建层计数（边沿/块/剔除），口径与 GATE 报告一致。
  if (!_measuring && (int32_t)(millis() - _idleHintMs) >= 0) {
    if (s_edgeTotal > 0 && _nEdgePairs == 0) {
      Display.centerUtf8(56, "只收到单沿,查极性");
    } else if (_idleEvSeen > 0) {
      Display.centerUtf8(56, "请短按开始测量");
    } else if (_nEdgePairs > 0) {
      Display.centerUtf8(56, "已遮到,未计数");
    }
  }

  if (_sgSrc == "CALC" && _sgTs > 0) {
    Display.setFont(DisplayService::FONT_TINY);
    Display.rightStr(10, Time.ageShort(_sgTs));   // 右上角数据年龄
  }

  Display.commit();
}

// ===================== BLE 菜单（分册02第6节） =====================

bool PendulumApp::onBleMsg(const String& line, String& reply) {
  HistoryStore hist("app2", HIST_CAP);

  // 顶层别名（PC 波形工具 's' 键直发，无需先进 [2] 会话）。
  // 必须放在 BS_NONE 菜单入口门之前：无会话时 _bs==BS_NONE 分支会先 return
  // false，本块若放在其后将永远不可达——fw_ble 的 STREAM 路由遍历所有 owner
  // 都收到 false，最终误报「未知 STREAM 命令」（R2-03 补丁原位置存在此漏洞）。
  if (line.startsWith("STREAM")) {
    if (line != "STREAM ON" && line != "STREAM OFF") return false;  // 未知别名，交还上层报错
    bool want = (line == "STREAM ON");
    if (want && Manager.active() != this) {
      // 程序2挂起时 onTick 停摆，开了流也不会有任何帧；直接给出可操作的指引
      reply = "ERR: 当前程序不是单摆测g，请长按切到 [2] 后再开波形";
      return true;
    }
    if (want == _streaming) {
      reply = String("INFO: 波形流已处于 ") + (_streaming ? "ON" : "OFF") + " 状态";
      return true;
    }
    if (want) {
      _streaming = true;
      _streamSeq = 0;
      _lastStreamMs = 0;
      Ble.streamBegin();
      reply = "OK: 波形流已启动（需程序2活跃 + 测量中 + BLE已连；发 STREAM OFF 停止）";
    } else {
      _streaming = false;
      Ble.streamEnd();
      reply = "OK: 波形流已停止（共推送 " + String(_streamSeq) + " 帧）";
    }
    return true;
  }

  // ---- 模拟数据注入（无光电门自测，R5 新增）----
  // 顶层命令：SIM ON / SIM OFF / SIM <半周期秒>
  // 生产已知半周期样本送入同一道环形队列，复用真实测量/波形/导出/PC 全链路。
  if (line.startsWith("SIM")) {
    String arg = line.substring(3);
    arg.trim();
    if (arg == "ON" || arg.length() == 0) {
      if (Manager.active() != this) {
        reply = "ERR: 当前程序不是单摆测g，请长按切到 [2] 后再开模拟数据";
        return true;
      }
      // 默认按 L 与 g=9.80 反算理论半周期（half = π√(L/g)），使测出 Gavg≈9.80 便于验证
      _simHalfUs = (uint32_t)(1e6 * 3.14159265358979323846 * sqrt(_pendLen / 9.80));
      _simOn = true;
      _simLastUs = micros();
      resetMeasure();
      _measuring = true;            // 自测免按键：自动开始测量
      Serial.printf("[Pend] SIM ON halfUs=%lu (T=%.4fs, expect g≈9.80)\n", (unsigned long)_simHalfUs, 2.0*_simHalfUs/1e6);
      reply = "OK: 模拟数据已开启（半周期≈" + String(_simHalfUs/1e6, 4) + "s, T≈" +
              String(2.0*_simHalfUs/1e6, 4) + "s, 理论 g≈9.80）；正在测量";
    } else if (arg == "OFF") {
      if (_measuring) { _measuring = false; finalizeMeasure(); }
      _simOn = false;
      Serial.println("[Pend] SIM OFF");
      reply = "OK: 模拟数据已关闭";
    } else {
      // 自定义半周期（秒）
      double h = arg.toDouble();
      if (h <= 0.05 || h > 10.0) {
        Ble.replyERR("半周期超出范围（0.05-10.0 s）");
        return true;
      }
      if (Manager.active() != this) {
        reply = "ERR: 当前程序不是单摆测g，请长按切到 [2] 后再开模拟数据";
        return true;
      }
      _simHalfUs = (uint32_t)(h * 1e6);
      _simOn = true;
      _simLastUs = micros();
      resetMeasure();
      _measuring = true;
      Serial.printf("[Pend] SIM ON (custom) halfUs=%lu\n", (unsigned long)_simHalfUs);
      reply = "OK: 模拟数据已开启（半周期=" + String(h, 4) + "s）；正在测量";
    }
    if (_bs == BS_NONE) { Ble.beginSession(this); }  // [3] 导出等命令需会话路由（可选）
    return true;
  }

  // ---- 采集层体检（GATE）----
  // 顶层命令，随时可用（不要求测量态）：GATE / GATE RESET
  // 排查「传感器遮住了却不计数」的第一入口，串口与 BLE 同语义。
  if (line.startsWith("GATE")) {
    String arg = line.substring(4);
    arg.trim();
    if (arg.startsWith("RESET")) {
      noInterrupts();
      s_edgeTotal = 0; s_ovfTotal = 0;
      interrupts();
      // 重建层计数是普通成员，不与 ISR 竞争
      _nEdgePairs = 0; _mergedBlk = 0; _nRejected = 0; _nRecovInt = 0;
      _nOutlier = 0; _nTimeout = 0; _blkTypicalUs = 0; _nStuck = 0;
      _nShortDrop = 0; _outlierWarned = false;
      _pendShort = 0; _hasShort = false;
      _blkOpen = false; _midValid = false;
      _pendSegEndUs = 0; _pendSegDur = 0; _pendSegMidUs = 0; _pendSegValid = false; _hasReplay = false;
      _psTIn = 0; _psEnd = 0; _psValid = false;
      _degraded = true;   // R7c 修订：默认宽容
      Serial.println("[Pend][GATE] 采集/重建诊断计数已清零（宽容模式）");
      reply = "OK: 采集诊断已清零（摆动几次后再发 GATE 即可得到本轮数据）";
      return true;
    }
    gateDiagReport("manual");
    reply = "OK: 采集诊断已输出；串口行以 [Pend][GATE:manual] 开头（另发 FIX 看纠错账目）";
    return true;
  }

  // ---- 纠错报告（FIX）----
  // 2026-10-03 新增。「g 算出来不对」时先发这条：它把剔除/恢复/删除/倍率纠正
  // 全部列出，并指出偏差到底来自丢样还是摆长填错。
  if (line.startsWith("FIX")) {
    fixDiagReport();
    reply = "OK: 纠错报告见串口 [Pend][FIX] 三行（已同步 BLE）";
    return true;
  }

  // ---- 原始边沿转储（EDGES）----
  // 2026-10-03 新增。逐条打印采集到的边沿（时刻+电平），
  // 用来证明「每一次亮起或熄灭都被记录」——数 ↓ 的个数就是真实摆动次数。
  if (line.startsWith("EDGES")) {
    edgesDump();
    reply = "OK: 原始边沿已输出到串口 [Pend][EDGES]（已同步 BLE）";
    return true;
  }

  // ---- 菜单入口 ----
  if (_bs == BS_NONE) {
    if (line == "2") {
      _bs = BS_LEN_ROOT;
      Ble.replyINFO("=== 摆长 / 实时波形 ===");
      Ble.replyINFO("[2.1] 查看当前 L");
      Ble.replyINFO("[2.2] 修改 L（0.1-5.0 m）");
      Ble.replyINFO("[2.4] 实时波形（STREAM ON/OFF）");
      Ble.replyINFO("（隐藏）altstat = 快慢交替诊断");
      Ble.replyINFO("（隐藏）GATE = 采集层体检（边沿/块/剔除/溢出）");
      Ble.replyINFO("（隐藏）FIX = 纠错账目（恢复/删除/倍率纠正）");
      Ble.replyINFO("（隐藏）EDGES = 原始边沿转储（验证零丢弃）");
      Ble.replyINFO("q 退出");
      Ble.beginSession(this);
      return true;
    }
    if (line == "3") {
      _bs = BS_HIST_ROOT;
      Ble.replyINFO("=== 历史数据 ===");
      Ble.replyINFO("[3.1] 预览");
      Ble.replyINFO("[3.2] 导出全部");
      Ble.replyINFO("[3.3] 清空（需键入 CLEAR 确认）");
      Ble.replyINFO("q 退出");
      Ble.beginSession(this);
      return true;
    }
    return false;   // 非本程序命令
  }

  // 全局退出：任何会话状态下 q 都能退出
  // （修复：原 BS_LEN_INPUT 提示"q 退出"但未处理，键入 q 时被当成 0 值判越界而卡死）
  if (line == "q") {
    reply = "INFO: 退出";
    _bs = BS_NONE;
    Ble.endSession();
    return true;
  }

  // ---- [2] 摆长会话 ----
  switch (_bs) {
    case BS_LEN_ROOT:
      if (line == "2.1" || line == "1") {
        reply = "INFO: L=" + String(_pendLen, 3) + " m" +
                "  n=" + String(_nHalf) + "  参与平均=" + String(_nPair * 2) +
                (_hasPend ? " (尚有1个落单待配对)" : "") +
                "  G=" + String(_Gavg, 4);
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      // 隐藏命令：altstat —— 查看快慢交替诊断（判断光电门是否偏离最低点）
      if (line.startsWith("altstat")) {
        reply = "INFO: 交替=" + String(_altDetected ? "已检出" : "未检出") +
                " 幅度=" + String(_altAmp, 1) + "%" +
                " 次数=" + String(_altCnt) +
                " | n=" + String(_nHalf) + " 参与=" + String(_nPair * 2) +
                " 落单排除=" + String(_nDropped) +
                " | G=" + String(_Gavg, 4) + " T=" + String((_nPair ? _sumPair / _nPair : 0), 4) + "s";
        if (_altDetected) {
          Ble.replyINFO("提示：交替说明光电门可能不在最低点；Gavg 已按快慢成对平均，无偏。");
        }
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      if (line == "2.2" || line == "2") {
        Ble.replyINFO("请输入新摆长（m，0.1-5.0）：");
        _bs = BS_LEN_INPUT;
        return true;
      }
      if (line == "2.4" || line == "4") {
        _bs = BS_STREAM_ROOT;
        Ble.replyINFO("=== 实时波形 ===");
        Ble.replyINFO("[2.4] 切换 ON/OFF（当前 " + String(_streaming ? "ON" : "OFF") + "）");
        Ble.replyINFO("q 退出");
        return true;
      }
      // 隐藏命令：alt <米> —— 修改海拔并立即重算当地标准 g（分册02第5节）
      if (line.startsWith("alt")) {
        String vs = line.substring(3);
        vs.trim();
        double alt = vs.toDouble();
        if (vs.length() == 0 || alt < -500.0 || alt > 9000.0) {
          Ble.replyERR("海拔超出范围（-500 ~ 9000 m）；格式：alt 123.4");
          return true;
        }
        Store.setDouble("app2.alt_m", alt);
        computeSg();                    // 按新海拔重算 sG 并写缓存
        _needDraw = true;
        reply = "OK: 海拔=" + String(alt, 1) + " m，sG 已重算为 " + String(_sG, 3);
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      goto quit;

    case BS_LEN_INPUT: {
      double v = line.toDouble();
      if (v < 0.1 || v > 5.0) {
        Ble.replyERR("超出范围（0.1-5.0），重新输入或 q 退出");
        return true;
      }
      _pendLenNew = v;
      Ble.replyINFO("L=" + String(v, 3) + " m，确认？(y/n)");
      _bs = BS_LEN_CONFIRM;
      return true;
    }

    case BS_LEN_CONFIRM:
      if (line == "y" || line == "Y") {
        _pendLen = _pendLenNew;
        Store.setDouble("app2.pend_len", _pendLen);
        reply = "OK: L 已更新为 " + String(_pendLen, 3) + " m";
        _bs = BS_NONE; Ble.endSession();
        _needDraw = true;               // OLED 立即刷新（分册02第6节）
        return true;
      }
      reply = "INFO: 已取消";
      _bs = BS_NONE; Ble.endSession();
      return true;

    // ---- [2.4] 实时波形 ----
    case BS_STREAM_ROOT:
      if (line == "2.4" || line == "4") {
        if (_streaming) {
          _streaming = false;
          Ble.streamEnd();
          reply = "OK: 波形流已停止（共推送 " + String(_streamSeq) + " 帧）";
        } else {
          _streaming = true;
          _streamSeq = 0;
          _lastStreamMs = 0;
          Ble.streamBegin();
          reply = "OK: 波形流已启动（需程序2活跃+测量中+BLE已连；发 2.4 再次停止）";
        }
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      goto quit;

    // ---- [3] 历史会话 ----
    case BS_HIST_ROOT:
      if (line == "3.1" || line == "1") {
        int n = hist.count();
        Ble.replyINFO("共 " + String(n) + " 组");
        if (n > 0) {
          String last;
          hist.getAt(n - 1, last);
          Ble.replyINFO("最新: " + last);
        }
        reply = "";                      // 已直接发送
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      if (line == "3.2" || line == "2") {
        int n = hist.count();
        if (n == 0) { reply = "ERR: 无历史数据"; _bs = BS_NONE; Ble.endSession(); return true; }
        startExport();
        reply = "INFO: 开始导出 " + String(n) + " 组…";
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      if (line == "3.3" || line == "3") {
        Ble.replyINFO("键入 CLEAR 确认清空（其他键取消）：");
        _bs = BS_HIST_CLEAR;
        return true;
      }
      goto quit;

    case BS_HIST_CLEAR:
      if (line == "CLEAR") {
        hist.clear();
        reply = "OK: 历史已清空";
      } else {
        reply = "INFO: 已取消";
      }
      _bs = BS_NONE; Ble.endSession();
      return true;

    default:
      goto quit;
  }

quit:
  reply = "INFO: 退出";
  _bs = BS_NONE;
  Ble.endSession();
  return true;
}

// PC 断连 / 会话被接管：复位本程序会话状态机。
// 不复位的话，重连后收到的第一条命令会按残留状态解释（例如再发 "2" 直接
// 被当成摆长输入而非打印菜单），表现为「连上以后命令全乱」。
void PendulumApp::onBleAbort() {
  _bs = BS_NONE;
  _exporting = false;
  if (_streaming) {
    _streaming = false;                 // 仅清标志：此时不应再向已断开的链路发 #STREAM END
  }
}

void PendulumApp::startExport() {
  HistoryStore hist("app2", HIST_CAP);
  _exportCnt = hist.count();
  _exportIdx = 0;
  _exporting = true;
  Ble.exportBegin(_exportCnt);           // #EXPORT BEGIN,<count>
}
