#pragma once
// =====================================================================
// fw_types.h —— 程序生命周期契约 + 全局事件总线（总纲第 5.2 / 5.3 节）
// =====================================================================
#include <Arduino.h>
#include <vector>

// ---------- 程序槽位（总纲 5.1 注册表） ----------
enum class AppId : uint8_t {
  PET      = 1,   // 程序1 桌宠屏保（默认·开机·屏保·home）
  PENDULUM = 2,   // 程序2 单摆测g
  WEATHER  = 3    // 程序3 实时天气
};

// ---------- 全局事件（总纲 5.3 事件总线） ----------
enum class EventType : uint8_t {
  EVT_NET_GOT,          // WiFi 已连接
  EVT_NET_LOST,         // WiFi 断开
  EVT_BLE_CONN,         // BLE 已连接
  EVT_BLE_DISC,         // BLE 已断开
  EVT_MEASURE_DONE,     // 程序2测量完成（value = 本次 g）
  EVT_WX_UPDATE_OK,     // 程序3天气更新成功
  EVT_WX_UPDATE_FAIL    // 程序3天气更新失败（code = 错误码）
};

struct Event {
  EventType type;
  float     value = 0;   // EVT_MEASURE_DONE：本次 g
  int       code  = 0;   // EVT_WX_UPDATE_FAIL：错误码（E_NET_xxx 等）
};

// =====================================================================
// App —— 所有程序实现同一组生命周期钩子（总纲 5.2 契约）
// 铁律：任何钩子单次 ≤50ms；联网/写Flash/导出一律异步回调（总纲 8.1）
// =====================================================================
class App {
public:
  virtual ~App() {}
  virtual AppId     id()   const = 0;
  virtual const char* name() const = 0;

  virtual void onCreate() {}                       // 首次激活前：NVS恢复状态、申请常驻资源；禁止阻塞联网
  virtual void onShow()   {}                       // 成为活跃：申请屏幕、挂载中断/定时器、重绘首屏；禁止长循环
  virtual void onHide()   {}                       // 失去活跃：状态写NVS、注销中断/定时器、归还屏幕（同步完成）
  virtual void onButton() {}                       // 短按转发：程序内主功能
  virtual void onTick(float dtSec) {}              // 活跃期间 ≤10Hz；单次 >50ms 禁止
  virtual void onEvent(const Event& evt) { (void)evt; }  // 订阅的全局事件到达

  // BLE 行文本路由；返回 false 表示本程序未处理（该行非本程序菜单命令）
  virtual bool onBleMsg(const String& line, String& reply) { (void)line; (void)reply; return false; }

  // BLE 会话被外部强制中止（PC 断连 / 管理器接管）：程序须复位自己的会话状态机 _bs，
  // 否则重连后第一条命令会被残留状态吞掉（如再发 "2" 直接跳进摆长输入态而非菜单）
  virtual void onBleAbort() {}

  // 测量等"不可被打断/不触发屏保"的忙碌态（程序2使用；屏保豁免条件）
  virtual bool isBusy() const { return false; }

  // 熄屏唤醒当次由管理器调用（不转发 onButton 主功能，总纲分册01第4节）
  virtual void onWake() {}
};

// =====================================================================
// EventBus —— 管理器广播，所有已注册程序接收（订阅式语义由程序自行过滤）
// =====================================================================
class EventBus {
public:
  static void subscribe(App* app);                 // 仅由 AppManager.registerApp 调用
  static void broadcast(const Event& evt);         // 同步派发到全部已注册程序
};
