#pragma once
// =====================================================================
// app3_weather.h —— 程序3「实时天气」（分册 03）
// 数据源：和风天气 QWeather v7（GZIP 响应；经度在前纬度在后）
// 四屏：①气温 ②天气+图标 ③风速 ④未来6h降水点阵（短按循环）
// 缓存：app3.wx_now / wx_now_ts / wx_hourly / wx_hourly_ts / wx_page
//       wx_hourly 为紧凑格式「fxTime|temp|pop;…」（约600B，NVS 单值 ~4KB 上限装不下原始 24h JSON）
//       显示一律读缓存（读写路径唯一）；>24h 年龄加 "!"
// BLE：[4] 天气（状态/改坐标/立即更新/配 Key+Host）
// 策略：拉取仅发生在本程序活跃时（保证非活跃零阻塞、不抢屏）；
//       失败绝不切屏，只更新角标 + 广播 EVT_WX_UPDATE_FAIL
// =====================================================================
#include "fw_types.h"

class WeatherApp : public App {
public:
  AppId     id()   const override { return AppId::WEATHER; }
  const char* name() const override { return "天气"; }

  void onCreate() override;          // 恢复 wx_page/缓存；注册 BLE 菜单
  void onShow()   override;          // 重绘上次屏；到期则调度拉取
  void onHide()   override;          // 切走：终止本轮拉取状态机（非活跃零网络请求）
  void onButton() override;          // 下一屏并写 wx_page
  void onTick(float dt) override;    // 执行到期的后台拉取 + 心跳刷新角标
  void onEvent(const Event& evt) override;
  bool onBleMsg(const String& line, String& reply) override;
  void onBleAbort() override;        // PC 断连 / 会话被接管：复位 _bs
  bool isBusy() const override { return false; }

private:
  enum BleState : uint8_t { BS_NONE, BS_ROOT, BS_COORD, BS_HOST, BS_KEY };
  // 拉取阶段机（2026-10-02 异步化）：原 doUpdate() 一次性阻塞 15-25s，
  // 现拆成「等连接 → 拉 now → 拉 24h」三段，每段在 onTick 里推进一小步。
  enum FxStage : uint8_t { FX_IDLE, FX_WAIT_NET, FX_NOW, FX_24H };

  void schedule();
  bool due();
  void advanceFx();                   // 状态机推进（非阻塞）
  bool startStage(FxStage st);        // 发起某阶段的网络请求
  String buildUrl(const char* path);  // 拼 URL（含凭据/坐标读取与兜底）
  String resolveHost();               // 读凭据 + 兜底 + 规范化（供 DNS 预检用）
  void updateFailed();           // 失败广播 + 30s 重试1次调度
  void draw();
  void drawIcon(int x, int y, const String& text);

  int      _page = 1;                // 1-4
  bool     _pending = false;         // 已调度待拉取
  uint32_t _lastConnReqMs = 0;       // 上次发起 WiFi 连接请求时刻（30s 防振荡）
  bool     _triedOnce = false;       // 本轮已重试过 1 次（分册03：失败重试1次/30s）
  uint32_t _retryAtMs = 0;
  int      _lastErr = 0;             // 最近错误码（0=无）
  uint32_t _lastHeartbeat = 0;
  bool     _needDraw = true;

  FxStage  _fx = FX_IDLE;            // 当前拉取阶段
  bool     _fxActive = false;        // 本轮拉取流程是否已启动（避免重复进入）

  // BLE 会话
  BleState _bs = BS_NONE;
  String   _tmpHost;
};
