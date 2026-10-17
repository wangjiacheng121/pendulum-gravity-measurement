#pragma once
// =====================================================================
// app2_pendulum.h —— 程序2「单摆测g」（分册 02 + 中点法修订补丁 + 2026-10-03 采集/重建分层）
// 职责：光电门测周期 → 计算 g（单次与平均）、当地标准 g、OLED 四行显示、
//       BLE 菜单（摆长/历史/导出/实时波形）
// 分层（2026-10-03）：ISR 纯采集边沿、零判决；loop 纯重建 + 全部纠错。
// 硬件：GPIO4 中断 CHANGE 双沿（空闲高/遮挡低），micros() 计时，遮挡中点为计时点。
// 门限（fw_config.h）：电气尖峰 200µs（R7b）、粘连块 0.50×hTheo（R7b）、片段合并 0.45×hRef、
//       中点相对门限 0.30×hTheo、半样本配对合并（双遮挡救回）、离群 0.25×hRef、
//       整数倍恢复 ±8%、系统性倍率纠正 m≤4。
// NVS：app2.pend_len / sg_cache / sg_src / sg_ts / alt_m / hist_00..49
// BLE：[2] 摆长（查看/修改）  [3] 历史数据（预览/导出/清空）  [2.4] 实时波形
//      顶层隐藏命令：GATE（门限体检）/ FIX（纠错报告）/ EDGES（原始边沿转储）
// =====================================================================
#include "fw_types.h"
#include "fw_config.h"   // PEND_LEN_DEFAULT / SG_DEFAULT / 全部门限

class PendulumApp : public App {
public:
  AppId     id()   const override { return AppId::PENDULUM; }
  const char* name() const override { return "单摆测g"; }

  void onCreate() override;          // 恢复 pend_len/sg_*/hist；注册 BLE 菜单
  void onShow()   override;          // 挂载 GPIO4 中断、重绘首屏
  void onHide()   override;          // 注销中断（测量累计值保留 RAM，切回续测）
  void onButton() override;          // 开始/停止测量
  void onTick(float dt) override;    // 重建+纠错、刷新显示、分块导出
  void onEvent(const Event& evt) override;
  bool onBleMsg(const String& line, String& reply) override;
  void onBleAbort() override;        // PC 断连 / 会话被接管：复位 _bs 并停导出/波形
  // 导出/波形期间同样豁免 L0 屏保：否则 180s 无操作被切回桌宠，
  // onTick 停摆 → 导出卡在中途（#EXPORT END 永不到达）/ 波形流断更
  bool isBusy() const override { return _measuring || _exporting || _streaming; }

private:
  enum BleState : uint8_t { BS_NONE, BS_LEN_ROOT, BS_LEN_INPUT, BS_LEN_CONFIRM,
                            BS_HIST_ROOT, BS_HIST_CLEAR,
                            BS_STREAM_ROOT };
  void resetMeasure();
  void finalizeMeasure();            // 停止：写历史 + 广播 EVT_MEASURE_DONE
  // —— 重建层处理函数（R7b 抽取：一个边沿最多触发两段片段处理
  //    （暂存半途片确认 + 当前块闭合），统计流必须可逐个消费）——
  bool gateProcessSeg(uint32_t sTIn, uint32_t sEnd, bool partial,
                      double hTheo, double hRef, uint32_t hRefUs, uint32_t hTheoUs,
                      uint32_t now);   // 片段 → 剔除/合并/发布；发布即调 processReplay
  bool gateProcessReplay(double hTheo, double hRef, uint32_t hRefUs, uint32_t hTheoUs,
                         uint32_t now); // 统一统计流：挂起/门限/恢复/离群/配对/G/纠正/波形
  void gateDiagReport(const char* tag);// 门限体检报告（串口+BLE 共用）
  void fixDiagReport();              // 纠错报告：本次丢样/恢复/删除统计
  void edgesDump();                  // 原始边沿转储（证明零丢弃）
  void computeSg();                  // 国际重力公式 + 缓存
  void draw();
  void startExport();

  // —— 重建层：边沿 → 遮挡片段 → 中点（2026-10-03）——
  // 这些变量是「重建状态机」，不是诊断计数器：它们决定了下一个中点怎么算。
  // ISR 不再持有它们，因此切走/复位时无需与 ISR 竞争，改动比旧实现更安全。
  bool     _blkOpen   = false;       // 正处于一次遮挡中（等出沿）
  uint32_t _blkTIn    = 0;           // 本次遮挡起始时刻
  bool     _midValid  = false;       // 是否已有一个可用作基准的中点
  uint32_t _lastMidUs = 0;           // 上一中点
  uint32_t _blkTypicalUs = 0;        // 典型遮挡时长（滑动均值，供体检用）
  // —— 同一次过零的碎片合并（2026-10-03）——
  // 挡光片边缘抖动 / 电平转换 RC 会把一次过零抖成几片，片间间隔在 ms 级。
  // 远小于半周期，物理上不可能是两次独立过零，必须合并成 1 个中点，
  // 否则会凭空多记摆动次数、把周期压短。判据用相对门限 0.30×hRef。
  // —— pending 发布制（R7-07，2026-10-06）——
  // 闭合片段先进 pending 缓冲；下一片段到来时先判同源合并（<0.45×hRef）：
  // 合并则加权并入 pending（不发布），不同源则把 pending 发布进统一流。
  // 旧实现把每个片段的中点立刻送进统一流，合并修正中点会再次进入形成
  // 「残影假样本」并把 _lastMidUs 推进 Δ/2，后续真实间隔全部偏短
  // （仿真 S3 复现 G=13.6，对应历史记录 G=17~25 一类杂乱值）。
  uint32_t _pendSegEndUs = 0;         // pending 片段结束时刻
  uint32_t _pendSegDur   = 0;         // pending 片段时长（合并加权基准）
  uint32_t _pendSegMidUs = 0;         // pending 片段中点
  bool     _pendSegValid = false;     // pending 是否有效
  // —— 半样本配对合并（R7-03，2026-10-06）——
  // 双遮挡（摆球+挡光片先后过槽）在 Δ 较大时无法在片段层合并，中点间隔呈
  // Δ 与 h−Δ 两簇交替。短样本（<0.80×hTheo）先挂起，与下一短样本配对：
  // 和 ≈ hTheo → 合并成 1 个真样本（时间守恒）；落单 → 删除留账。
  double   _pendShort  = 0;           // 挂起的短样本（秒），配合 _hasShort
  bool     _hasShort   = false;
  uint32_t _nShortDrop = 0;           // 落单删除的短样本数（诊断口径）
  // —— 半途片暂存（R7b，2026-10-06：毛刺截断防御）——
  // LOW 期间又见 LOW 时，旧实现立即结算半途片 —— 但触发它的可能是一个
  // 微秒级毛刺 LOW（粘连段内 220Hz 振荡），它把粘连块肢解成 4.5ms 假片
  // 混进中点流。现在半途片先进暂存区，延迟一个边沿确认：
  //   下一个出沿距暂存末端 < 200µs → 触发 LOW 是毛刺 → 作废暂存片并把
  //   会话恢复到粘连开头（毛刺无权截断正在进行的遮挡）；
  //   否则半途片确认有效，走正常处理。
  uint32_t _psTIn   = 0;              // 暂存半途片起始（= 被截断 LOW 的起点）
  uint32_t _psEnd   = 0;              // 暂存半途片结束（= 截断它的 LOW 时刻）
  bool     _psValid = false;
  // —— 统一统计流的入口 ——
  // 所有中点（含合并后的）都从这里走一遍，避免出现「某条路径漏更新某个量」。
  uint32_t _replayMid = 0;
  bool     _hasReplay = false;

  // —— 重建层统计（全部是「计数 + 决策依据」，缺一不可）——
  uint32_t _nEdgePairs  = 0;         // 重建出的完整遮挡片段数
  uint32_t _mergedBlk   = 0;         // 被判为同一次过零碎片而合并的片段数
  uint32_t _nRejected   = 0;         // 被判电气尖峰/过近而剔除的中点数
  uint32_t _nStuck      = 0;         // 粘连/振荡假块剔除数（R7b：dur>0.50×hTheo 的"超长遮挡"）
  uint32_t _nRecovInt   = 0;         // 整数倍恢复补回的样本数
  uint32_t _nOutlier    = 0;         // 离群删除数
  uint32_t _nTimeout    = 0;         // 超时重置基准次数（停摆/重启）
  bool     _outlierWarned = false;   // 离群连续删除已达提示阈值（只提示一次）
  double   _lastFixK    = 1.0;       // 最近一次系统性倍率纠正系数（1=未纠正）
  uint32_t _fixApplied  = 0;         // 系统性纠正已应用次数
  // —— 宽容降级模式（R7c，2026-10-06）——
  // R7c 修订（用户决策）：**默认即宽容**，不再先严格后降级——用户实验不允许
  // 「先等 30s 零产出才出数」。片段层门限直接取宽容值（BLINK 20µs、STUCK 停用），
  // 统计层质检保留。保留 _degraded 标志位供 GATE 显示与将来恢复严格模式。
  bool     _degraded   = true;       // 宽容模式（默认开启；GATE RESET/重开测量仍保持）

  // —— 测量状态（RAM，切程序保留）——
  bool     _measuring = false;
  // —— 模拟数据源（无光电门自测）：onTick 按计时器生产已知半周期，复用同一道边沿队列 ——
  bool     _simOn = false;           // 模拟数据开关
  uint32_t _simLastUs = 0;           // 下次应产生样本的时刻（micros）
  uint32_t _simHalfUs = 1000000;     // 模拟半周期（µs），默认 1.0s → 理论 T=2.0s
  int      _nHalf = 0;               // 半周期个数（显示字段 N）
  double   _sumHalf = 0;             // 半周期累加（秒）
  double   _lastT = 0, _lastG = 0, _Gavg = 0;

  // —— 快慢交替检测与配对纠正（2026-10-02）——
  // 光电门不在最低点时，相邻半周期一快一慢（幅度对称），两两相加恒等于真周期。
  // 因此把累计口径改为「按对结算」：每收满 2 个半周期才计入 _nPair/_sumPair，
  // 落单的那一个不进平均 → 无论 n 奇偶、是否丢样本，g 都无偏。
  int      _nPair = 0;               // 已配对的半周期对数
  double   _sumPair = 0;             // 配对累加（每对 = 一个完整周期，秒）
  double   _pendHalf = 0;            // 待配对的落单半周期（秒），0 = 无
  bool     _hasPend = false;
  bool     _altDetected = false;     // 是否检出快慢交替（光电门可能偏离最低点）
  double   _altAmp = 0;              // 交替幅度（相对均值 %），供屏幕标注
  uint32_t _altCnt = 0;              // 检出交替的次数
  int      _nDropped = 0;            // 因落单被排除的样本数
  double   _lastPairT = 0;           // 最近一个配对算出的周期（秒）

  // —— 待机防呆（2026-10-02）——
  // 待机时 onTick 会把重建出的中点直接丢弃（`if (!_measuring) continue`），这是设计行为。
  // 但用户看到「摆了很多次 n 纹丝不动」会误判硬件坏了。
  // 故在待机时统计重建出的摆动次数，一旦有摆动就提示「请短按开始测量」。
  uint32_t _idleEvSeen = 0;          // 待机期间重建出的摆动次数
  int32_t  _idleHintMs = 0;          // 提示显示截止时刻（环绕安全比较用）

  // —— 配置与缓存（NVS 恢复）——
  double   _pendLen = PEND_LEN_DEFAULT;
  double   _sG = SG_DEFAULT;
  String   _sgSrc = "DEFAULT";
  uint32_t _sgTs = 0;

  // —— BLE 会话 ——
  BleState _bs = BS_NONE;
  double   _pendLenNew = 0;
  bool     _exporting = false;
  int      _exportIdx = 0, _exportCnt = 0;

  // —— 实时波形流（phyphox 兼容，NUS #STREAM 协议）——
  bool     _streaming = false;         // 流推送中
  uint32_t _streamSeq = 0;             // 流序号（单调递增）
  uint32_t _lastStreamMs = 0;          // 上次推送时间（限频用）

  uint32_t _lastDrawMs = 0;
  uint32_t _lastDiagMs = 0;          // 门诊断心跳上次打印时刻
  bool     _needDraw = true;
};
