// =====================================================================
// fw_ble.cpp —— BLE 服务实现（NUS 透传 + 菜单路由 + WiFi 配网会话）
// =====================================================================
#include "fw_ble.h"
#include "fw_config.h"
#include "fw_store.h"
#include "fw_net.h"
#include "fw_time.h"
#include "fw_manager.h"
#include <nvs.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // PC→板，板端写
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // 板→PC，Notify

BleService Ble;

// ---------- 内部回调 ----------
static BLECharacteristic* s_txChar = nullptr;
static std::deque<String>* s_lines = nullptr;
static String* s_rxBuf = nullptr;
static BLEServer* s_server = nullptr;

// 连接参数自愈（R5-03）：实测 Windows 对连接时立刻发出的参数请求**偶尔不响应**
// （链路回落 4s 慢节奏，通知/吞吐近乎瘫痪）。记录当前 interval，tick 里发现
// 仍偏大就每 5s 重发一次请求，直到中央接受为止。
static uint16_t s_connHandle = 0xFFFF;
static uint16_t s_curItvl   = 0xFFFF;   // 当前连接间隔（1.25ms 单位），0xFFFF=未知
static uint32_t s_paramReqMs = 0;

// BLE 回调运行在独立的 Bluedroid 任务上下文，与 loop() 并发访问 _lines / _rxBuf。
// std::deque 与 String 均非线程安全：push_back 与 pop_front 交叠会破坏内部指针、
// 甚至损坏堆。此处用临界区做互斥（访问极短，不影响吞吐）。
static portMUX_TYPE s_rxMux = portMUX_INITIALIZER_UNLOCKED;

static void bleTx(const String& line) {
  if (!s_txChar) return;
  String out = line + "\n";
  if (out.length() > 180) out = out.substring(0, 180) + "\n";
  s_txChar->setValue((uint8_t*)out.c_str(), out.length());
  s_txChar->notify();
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    if (!s_rxBuf || !s_lines) return;
    String v = ch->getValue();
    portENTER_CRITICAL(&s_rxMux);          // 保护 _lines / _rxBuf（见 s_rxMux 注释）
    for (size_t i = 0; i < v.length(); i++) {
      char c = v[i];
      if (c == '\n') {
        if (s_rxBuf->length() > 0) {
          s_lines->push_back(*s_rxBuf);
          if (s_lines->size() > 16) s_lines->pop_front();
          s_rxBuf->clear();
        }
      } else if (c != '\r') {
        if (s_rxBuf->length() < 300) *s_rxBuf += c;   // 防溢出
      }
    }
    portEXIT_CRITICAL(&s_rxMux);
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    Ble._bleConn = true;
    Ble._connEvtPending = true;
    Serial.println("[BLE] connected");
  }
  // R5-01：上板实测发现通知约 4s/条（连接间隔被协商到规格上限 4000ms，
  // 导出 50 行要 3 分多钟、波形流完全不可用）。原因是控制器睡眠下外设
  // 不主动请求连接参数。R5-03 改为在 tick() 中链路稳定 ≥5s 后再请求 15–30ms。
  // —— 注意：原实现在这里「连接瞬间立即」请求参数，实测在部分 Windows 蓝牙栈上
  // 会被中央拒绝并直接断链（串口可见 [BLE] connected → params update status=19
  // → disconnected，约 1 秒内被远端掐断）。故连接建立期不再发参数更新，交由
  // tick() 在握手完成后再兜底请求（见下方 tick 内的 R5-03 逻辑）。
  void onConnect(BLEServer* pServer, ble_gap_conn_desc* desc) override {
    s_connHandle = desc->conn_handle;
    s_curItvl = desc->conn_itvl;
    (void)pServer;
  }
  // 观测中央对参数请求的接受结果（interval 单位 1.25ms）
  void onConnParamsUpdate(uint16_t conn_handle, uint16_t interval,
                          uint16_t latency, uint16_t timeout, uint8_t status) override {
    s_curItvl = interval;
    Serial.printf("[BLE] params update result: status=%u interval=%.1fms latency=%u timeout=%ums\n",
                  status, interval * 1.25, latency, timeout * 10);
  }
  void onDisconnect(BLEServer*) override {
    Ble._bleConn = false;
    Ble._discEvtPending = true;
    s_connHandle = 0xFFFF;
    s_curItvl = 0xFFFF;
    BLEDevice::startAdvertising();   // 常备可连接（总纲第 3 节）
    Serial.println("[BLE] disconnected, advertising restarted");
  }
};

void BleService::begin() {
  BLEDevice::init("pendulum-g");
  BLEDevice::setMTU(185);

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  s_server = server;

  BLEService* svc = server->createService(NUS_SERVICE_UUID);

  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rx->setCallbacks(new RxCallbacks());

  // 注意：不要再手动 addDescriptor(new BLE2902())。本核心的 BLE 库底层已是 NimBLE，
  // CCCD（0x2902）会在特征具备 NOTIFY 属性时**自动**添加；手动添加不但多余，
  // 且 BLE2902 已被标记为 deprecated（未来版本会移除）
  s_txChar = svc->createCharacteristic(
      NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  s_lines = &_lines;
  s_rxBuf = &_rxBuf;

  // 系统全局菜单（总纲第 6 节固定树）
  registerMenu("0",    "状态总览",       nullptr);
  registerMenu("1",    "WiFi 配网",      nullptr);
  // [2]摆长 [3]历史 由程序2注册；[4]天气 由程序3注册；[5]AI问答 由程序1注册
  Serial.println("[BLE] OK: NUS ready, name=pendulum-g");
}

void BleService::registerMenu(const String& key, const String& title, App* owner) {
  for (auto& m : _menu) { if (m.key == key) { m.title = title; m.owner = owner; return; } }
  MenuEntry e; e.key = key; e.title = title; e.owner = owner;
  _menu.push_back(e);
}

void BleService::sendLine(const String& prefixAndText) {
  Serial.println("[BLE>] " + prefixAndText);
  bleTx(prefixAndText);
}

// 串口命令通道（2026-10-02）：与 BLE RX 共用同一行队列，
// 使串口获得与手机/PC 完全相同的命令集（[0] 状态、各程序菜单、APP n 切程序、配网会话）。
// 入队必须在 s_rxMux 临界区内，与 Bluedroid 回调的 push_back 互斥（ESP32-S3 双核）。
void BleService::pushSerialLine(const String& line) {
  portENTER_CRITICAL(&s_rxMux);
  _lines.push_back(line);
  if (_lines.size() > 16) _lines.pop_front();   // 与 BLE 侧同上限，防串口刷屏挤爆队列
  portEXIT_CRITICAL(&s_rxMux);
}

void BleService::printMenu() {
  replyINFO("=== 菜单 ===");
  for (auto& m : _menu) {
    String tag = m.owner ? "" : " [系统]";
    replyINFO("[" + m.key + "] " + m.title + tag);
  }
  replyINFO("发送命令词执行；q 退出子菜单");
}

// 强制中止全部交互式会话（PC 断连 / 系统接管时调用）。
// 必须同时复位程序内的会话状态机：只清 _session 会让程序残留 _bs，
// 表现为「重连后发 [2] 不再打印菜单、而是直接跳进某个输入态」。
void BleService::abortSessions(const char* why) {
  if (_session) {
    _session->onBleAbort();
    _session = nullptr;
  }
  if (_ws != WS_OFF) {
    _ws = WS_OFF;
    Net.radioOffIfIdle();               // 配网会话被中断 → 若未在连网则关射频，避免常开耗电
  }
  if (why) Serial.printf("[BLE] sessions aborted (%s)\n", why);
}

void BleService::tick() {
  // 连接/断开事件延后到 loop 上下文广播（线程安全）
  if (_connEvtPending) { _connEvtPending = false; Event e; e.type = EventType::EVT_BLE_CONN; EventBus::broadcast(e); }
  if (_discEvtPending) {
    _discEvtPending = false;
    abortSessions("BLE disconnect");       // 先收拾，再广播（让程序看到干净状态）
    Event e; e.type = EventType::EVT_BLE_DISC; EventBus::broadcast(e);
  }

  // R5-03 连接参数自愈：连接间隔仍 >50ms（慢节奏）时每 5s 重发一次请求。
  // 实测 Windows 对连接瞬间的首次请求偶尔不响应，此兜底保证链路最终收敛到 15-30ms。
  if (_bleConn && s_connHandle != 0xFFFF && s_curItvl > 50 &&
      millis() - s_paramReqMs > 5000 && s_server) {
    s_paramReqMs = millis();
    s_server->requestConnParams(s_connHandle, 12, 24, 0, 400);
    Serial.printf("[BLE] conn params retry (cur=%.1fms)\n", s_curItvl * 1.25);
  }

  // WiFi 扫描进行中 → 完成后输出列表（异步，不阻塞）
  if (_ws == WS_SCANNING && Net.scanDone()) {
    int n = WiFi.scanComplete();
    if (n <= 0) {
      replyINFO("未扫描到网络");
    } else {
      replyINFO("扫描到 " + String(n) + " 个网络：");
      for (int i = 0; i < n && i < 15; i++) {
        replyINFO("[" + String(i) + "] " + Net.scanSsid(i) + " (" + String(Net.scanRssi(i)) + "dBm)");
      }
      if (n > 15) replyINFO("(仅显示前15个)");
    }
    replyINFO("回复序号选择网络，q 取消");
    // 关键：先把结果缓存到本地再释放——scanDelete 后 WiFi.scanComplete()/SSID() 即失效，
    // WS_SELECT 阶段再读会得到 WIFI_SCAN_FAILED，导致任何序号都被判"无效"
    _scanN = 0;
    for (int i = 0; i < n && i < 15; i++) _scanSsid[_scanN++] = Net.scanSsid(i);
    WiFi.scanDelete();
    // SSID 已缓存到本地，射频可以关掉了：否则一次扫描会把 WiFi 常开到下次配网为止，
    // 直接违反总纲 5.4 的 L2 空闲断电分级
    Net.radioOffIfIdle();
    _ws = WS_SELECT;
    return;
  }
  // 扫描兜底：底层失败（WIFI_SCAN_FAILED，如射频被占/模式冲突）时 scanDone()
  // 永远为 false，会话会卡在 SCANNING 且只有发 q 能出去——用户不知道要按 q。
  // 30s 无结果即判扫描异常，自动退出并提示重试（环绕安全比较）。
  if (_ws == WS_SCANNING && (int32_t)(millis() - _scanStartMs) > 30000) {
    replyINFO("扫描超时/失败，请重新发送 1.2 重试");
    _ws = WS_OFF;
    Net.radioOffIfIdle();
    return;
  }

  String line;                           // 取行必须与 RxCallbacks 的入队互斥（见 s_rxMux）
  {
    portENTER_CRITICAL(&s_rxMux);
    if (_lines.empty()) { portEXIT_CRITICAL(&s_rxMux); return; }
    line = _lines.front();
    _lines.pop_front();
    portEXIT_CRITICAL(&s_rxMux);
  }
  line.trim();
  if (line.length() == 0) return;
  Serial.println("[BLE<] " + line);

  // 应用交互会话：全部行转发给属主程序（程序2/3 多行输入）
  if (_session) {
    String reply;
    if (_session->onBleMsg(line, reply) && reply.length()) sendLine(reply);
    return;
  }

  if (_ws != WS_OFF) { handleWifiSession(line); return; }

  // HELP / 菜单
  if (line == "HELP" || line == "?" || line == "菜单") { printMenu(); return; }

  // 全局菜单：[0] 状态总览
  if (line == "0") {
    nvs_stats_t st;
    nvs_get_stats(nullptr, &st);
    String gSrc; Store.getString("app2.sg_src", gSrc);
    double gVal = Store.getDouble("app2.sg_cache", 0.0);
    replyINFO("固件 " FW_NAME " v" FW_VERSION " (" FW_BUILD_DATE ")");
    replyINFO("当前程序: " + String(Mgr_activeName()));
    replyINFO("运行时长: " + String(millis() / 1000) + "s");
    replyINFO("NVS: 已用 " + String(st.used_entries) + " / 剩余 " + String(st.free_entries) + " 条目");
    replyINFO("网络: " + Net.statusText());
    replyINFO("时间: " + Time.dateStr() + " " + Time.hhmm() + (Time.synced() ? " (已对时)" : " (未对时)"));
    replyINFO("当地g: " + String(gVal, 2) + " 来源=" + (gSrc.length() ? gSrc : "DEFAULT"));
    return;
  }

  // 全局菜单：[1] WiFi 配网
  if (line == "1") {
    replyINFO("=== WiFi 配网 ===");
    replyINFO("[1.1] 连接测试（用户凭据优先+内置默认，自动轮换）");
    replyINFO("[1.2] 扫描SSID列表→选序号→输密码→保存连接");
    replyINFO("[1.3] 查看连接状态");
    _ws = WS_ROOT;
    return;
  }

  // 顶层别名命令（不进子会话直接发送的诊断/控制类命令）。
  //
  // 背景：fw_ble 只对精确菜单键（"2"/"3"/…）与 STREAM 前缀做 owner 转发，
  // 其余自定义前缀会被当成「未知命令」丢弃 —— SIM 与 2026-10-03 新增的
  // GATE/FIX/EDGES 都踩过这个坑（实测：串口发 GATE 只会得到 ERR 未知命令）。
  // 根因是每加一个别名就要在这里补一段 if，漏一处就静默失效。
  // 故改为一张前缀表统一处理：命中的前缀转发给所有注册了菜单的程序，
  // 由各程序自己判断认不认识（onBleMsg 返回 false 即视为不认识，继续下一个）。
  // 新增别名只需在下面加一个字符串。
  static const char* kTopLevelPrefixes[] = {
    "STREAM",   // 实时波形 ON/OFF（PC phyphox_stream.py 的 's' 键直发）
    "SIM",      // 模拟数据 ON/OFF/<秒>（无光电门自测）
    "GATE",     // 采集层体检：边沿/溢出/片段/碎片合并/剔除
    "FIX",      // 纠错账目：恢复/删除/倍率纠正
    "EDGES",    // 原始边沿转储：验证采集层零丢弃
  };
  for (const char* pfx : kTopLevelPrefixes) {
    if (!line.startsWith(pfx)) continue;
    String reply;
    bool handled = false;
    for (auto& m : _menu) {
      if (!m.owner) continue;
      if (m.owner->onBleMsg(line, reply)) { handled = true; break; }
    }
    if (handled) {
      if (reply.length()) sendLine(reply);
    } else {
      // 明确列出可用形式，避免用户面对「未知命令」无从下手
      replyERR(String("未知 ") + pfx + " 命令（可用：" + pfx +
               (String(pfx) == "STREAM" ? " ON / OFF" :
                String(pfx) == "SIM"    ? " ON / OFF / <半周期秒>" :
                String(pfx) == "GATE"   ? " / RESET" : " / <n>") + "）");
    }
    return;
  }

  // 调试命令：APP <1|2|3> —— BLE 远程切程序（R5 调试用）。
  // 设计上切程序本应只走按键长按；但无按键调试时无法进入程序2/3，
  // 故开放此命令。走 Manager.activate 正规路径，生命周期钩子完整触发
  // （onHide/onShow 与按键切换完全一致），不会绕过任何收尾逻辑。
  if (line.startsWith("APP ")) {
    String arg = line.substring(4);
    arg.trim();
    int n = arg.toInt();
    AppId id;
    if (n == 1) id = AppId::PET;
    else if (n == 2) id = AppId::PENDULUM;
    else if (n == 3) id = AppId::WEATHER;
    else { replyERR("APP <1|2|3>"); return; }
    if (Manager.activeId() == id) { replyOK("已在 " + String(Mgr_activeName())); return; }
    // 切走前若有交互会话属主，先中止（与断连同等收尾）
    if (_session) { _session->onBleAbort(); _session = nullptr; }
    Manager.activate(id, "ble-debug");
    replyOK("已切换到 " + String(Mgr_activeName()));
    return;
  }

  // 程序菜单：精确匹配命令词，路由到 owner
  for (auto& m : _menu) {
    if (m.key == line && m.owner != nullptr) {
      String reply;
      if (m.owner->onBleMsg(line, reply)) {
        if (reply.length()) sendLine(reply);
      } else {
        replyERR("命令处理失败");
      }
      return;
    }
  }

  replyERR("未知命令: " + line);
  printMenu();
}

void BleService::handleWifiSession(const String& line) {
  switch (_ws) {
    case WS_ROOT:
      if (line == "1.1" || line == "1") {
        if (!Net.hasAnyCred()) { replyERR("无可用凭据，请先 1.2 配网"); return; }
        Net.requestConnect();
        replyOK("开始连接（凭据列表自动轮换）: " + Net.currentTarget());
        _ws = WS_OFF;
      } else if (line == "1.2" || line == "2") {
        replyINFO("扫描中，请稍候…");
        Net.startScan();
        _scanStartMs = millis();          // 供 30s 超时兜底（见 tick 扫描分支）
        _ws = WS_SCANNING;
      } else if (line == "1.3" || line == "3") {
        replyINFO(Net.statusText());
      } else if (line == "q") {
        replyOK("退出配网");
        _ws = WS_OFF;
        Net.radioOffIfIdle();            // q 路径统一关射频（补齐 R2-09）
      } else {
        replyERR("输入 1.1 / 1.2 / 1.3 或 q");
      }
      break;

    case WS_SCANNING:
      if (line == "q") { replyOK("已取消"); _ws = WS_OFF; Net.radioOffIfIdle(); }
      else replyINFO("扫描中，请稍候…");
      break;

    case WS_SELECT: {
      if (line == "q") { replyOK("已取消"); _ws = WS_OFF; Net.radioOffIfIdle(); break; }
      // 严格数字校验：String.toInt() 对非数字串返回 0，会把误输入当成「选择网络 0」
      bool allDigits = (line.length() > 0);
      for (size_t i = 0; i < line.length() && allDigits; i++) {
        if (!isDigit((unsigned char)line[i])) allDigits = false;
      }
      int n = allDigits ? line.toInt() : -1;
      if (n < 0 || n >= _scanN) {
        replyERR(_scanN ? ("序号无效(0-" + String(_scanN - 1) + ")") : "扫描结果已失效，请重新 1.2 扫描");
        break;
      }
      _selSsid = _scanSsid[n];
      replyINFO("已选: " + _selSsid);
      replyINFO("请发送 WiFi 密码（q 取消）:");
      _ws = WS_PASS;
      break;
    }

    case WS_PASS:
      if (line == "q") { replyOK("已取消"); _ws = WS_OFF; Net.radioOffIfIdle(); break; }
      if (line.length() < 8) { replyERR("密码至少8位"); break; }
      Net.connectWith(_selSsid, line, true);
      replyOK("凭据已保存，正在连接: " + _selSsid);
      _ws = WS_OFF;
      break;

    default:
      _ws = WS_OFF;
      break;
  }
}
