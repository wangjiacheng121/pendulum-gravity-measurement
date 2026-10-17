#pragma once
// =====================================================================
// fw_ble.h —— BLE 服务（总纲第 3、6 节）
// 契约：
//  - Nordic UART Service (NUS)，广播名 pendulum-g，常备可连接
//  - 文本按行传输，行尾 \n；回包 OK:/ERR:/INFO: 前缀
//  - 菜单注册表：系统注册全局项（[0][1]），各程序注册自己的项
//  - 导出流：#EXPORT BEGIN,<count> ... #EXPORT END（程序2 使用）
//  - 实时数据流：#STREAM <csv> / #STREAM BEGIN / #STREAM END（程序2 波形）
// =====================================================================
#include <Arduino.h>
#include <deque>
#include <vector>
#include "fw_types.h"

struct MenuEntry {
  String  key;      // 命令词（如 "0" / "5"）
  String  title;    // 菜单标题
  class App* owner; // nullptr = 系统全局项
};

class BleService {
public:
  // 连接状态标志（BLE 回调直接置位，tick 中转事件）
  // volatile 必需：这三个标志由 Bluedroid 任务写、loop 任务读，ESP32-S3 是双核
  volatile bool _bleConn = false;
  volatile bool _connEvtPending = false;
  volatile bool _discEvtPending = false;

  void begin();       // 初始化 NUS 从机并开始广播
  void tick();        // 行文本路由（在 loop 上下文执行，避免 BLE 任务里做重活）
  bool connected() const { return _bleConn; }

  // 串口命令通道（2026-10-02）：把一行塞进与 BLE 相同的行队列，
  // 从而复用全部现有路由（[0] 状态 / 各程序菜单 / APP n 切程序 / WiFi 配网会话），
  // 回复仍由 sendLine 打到串口（[BLE>] 前缀）。
  // 背景：本机 Windows 蓝牙栈对该设备 GATT 不兼容（手机 nRF Connect 可连），
  // 串口是唯一可靠的调试/验证通道。实现在 fw_ble.cpp（需访问静态 s_rxMux）。
  void pushSerialLine(const String& line);

  // 注册菜单项（App 在 onCreate 中调用自己的）
  void registerMenu(const String& key, const String& title, App* owner);

  // 板→PC 发送（自动补 \n）
  void sendLine(const String& prefixAndText);          // 原样发送（调用方自带 OK:/ERR:/INFO:）
  void replyOK(const String& s)   { sendLine("OK: " + s); }
  void replyERR(const String& s)  { sendLine("ERR: " + s); }
  void replyINFO(const String& s) { sendLine("INFO: " + s); }

  // 导出流包裹（程序2 历史导出用）
  void exportBegin(int count) { sendLine("#EXPORT BEGIN," + String(count)); }
  void exportData(const String& line) { sendLine(line); }
  void exportEnd() { sendLine("#EXPORT END"); }

  // 实时数据流（程序2 phyphox 波形推送用）
  void streamBegin()   { sendLine("#STREAM BEGIN"); }
  void streamData(const String& csv) { sendLine("#STREAM " + csv); }
  void streamEnd()     { sendLine("#STREAM END"); }

  // ---- 应用交互会话（程序2 摆长/历史、程序3 天气配设的多行输入）----
  // 进入后所有行文本转发给 owner->onBleMsg，直到 owner 调 endSession()
  void beginSession(App* owner) { _session = owner; }
  void endSession()             { _session = nullptr; }
  bool inSession() const        { return _session != nullptr; }

private:
  enum WifiSession : uint8_t { WS_OFF, WS_ROOT, WS_SCANNING, WS_SELECT, WS_PASS };
  void handleWifiSession(const String& line);
  void printMenu();
  void abortSessions(const char* why);     // 强制中止会话（PC 断连等）；含程序侧状态机复位

  std::deque<String> _lines;      // 收到的行队列（BLE 回调入队，tick 出队处理）
  String _rxBuf;

  std::vector<MenuEntry> _menu;
  WifiSession _ws = WS_OFF;
  String _selSsid;
  String _scanSsid[15];           // 扫描结果本地缓存（scanDelete 释放后仍可供选择，见 fw_ble.cpp）
  int    _scanN = 0;
  uint32_t _scanStartMs = 0;      // WS_SCANNING 起始时刻（扫描异常失败时 30s 兜底退出）
  App*   _session = nullptr;      // 当前交互会话属主（nullptr=无）
};

// 全局单例（定义在 fw_ble.cpp，供各程序/服务引用）
extern BleService Ble;
