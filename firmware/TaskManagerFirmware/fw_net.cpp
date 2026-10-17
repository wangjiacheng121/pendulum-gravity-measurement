// =====================================================================
// fw_net.cpp —— 网络服务实现
// =====================================================================
#include "fw_net.h"
#include "fw_config.h"
#include "fw_store.h"
#include "fw_time.h"
#include "fw_types.h"
#include <HTTPClient.h>
#include "fw_inflate.h"   // 自带 inflate（本工具链无 zlib，2026-10-02）

// 若工具链恰好带 zlib 则优先用它；本项目实测的 esp32:esp32 3.3.12-cn **不带**，
// 此时走自带 inflate（fw_inflate.h）。
#if __has_include(<zlib.h>)
  #define FW_HAS_ZLIB 1
  #include <zlib.h>
#endif

NetService Net;

// =====================================================================
// 异步 HTTP 的跨线程对象（2026-10-02）
// 必须在 begin() 与 httpStart() 之前定义 —— 它们是文件级静态对象，
// C++ 要求「先定义后使用」，原先放在文件尾部的 httpGetBlocking 段落里会编译失败。
// 结构：主循环 httpStart() 投请求 → 网络任务做阻塞活 → 主循环 httpPoll() 取结果。
// 跨线程数据用互斥量 + 二值信号量保护，同一时刻只允许一个在飞请求。
// 注意：网络任务里禁止调用 EventBus::broadcast（会碰 NVS/String，与主循环竞态），
//       连接状态机 tick() 仍只在主循环上下文跑。
// =====================================================================
struct HttpJob {
  char url[320];
  char host[128];
  bool gunzip;
  uint32_t seq;     // R6-01：请求序号，结果原样带回供 httpPoll 校验归属
};

static QueueHandle_t      s_httpQ    = nullptr;   // 主循环 → 网络任务
static SemaphoreHandle_t  s_httpMux  = nullptr;   // 保护 Net 的结果字段
static SemaphoreHandle_t  s_httpDone = nullptr;   // 网络任务 → 主循环（结果就绪）
static HttpJob            s_httpJob;              // 投递缓冲（入队后网络任务立刻取走）

void NetService::begin() {
  loadCreds();
  rebuildList();                     // 组装凭据列表：用户 NVS 凭据优先 + fw_config 内置默认
  WiFi.persistent(false);            // 凭据由我们自行存 NVS，避免 Flash 重复写入
  WiFi.mode(WIFI_OFF);
  WiFi.setSleep(true);
  _state = ST_IDLE;

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)info;
    // 注意：此回调运行在 WiFi 事件任务上下文，禁止直接 EventBus::broadcast
    // （onEvent 里会做 NVS 写/String 操作，与主循环竞态）——只置标志，tick 中延后广播
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.println("[Net] EVT_NET_GOT (deferred)");
        Net._evtGotPending = true;
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        // 凭据轮换期间的断开事件不广播（避免桌宠/天气把「换 AP」误判成掉线）
        if ((int32_t)(millis() - Net._suppressLostUntilMs) >= 0) {
          Serial.println("[Net] EVT_NET_LOST (deferred)");
          Net._evtLostPending = true;
        }
        break;
      default:
        break;
      }
  });
  Serial.printf("[Net] begin, user creds %s, fallback list %d AP(s)\n",
                _ssid.length() ? "saved" : "EMPTY", _listN);

  // ---- 异步 HTTP 网络任务（2026-10-02）----
  // 独立任务承载全部阻塞网络操作，主循环只投递请求/取结果。
  // 栈 6KB：HTTPClient + 1KB 读缓冲 + GZIP 解压工作区。
  if (!s_httpQ) {
    s_httpQ = xQueueCreate(2, sizeof(HttpJob));
    s_httpMux = xSemaphoreCreateMutex();
    s_httpDone = xSemaphoreCreateBinary();
    if (s_httpQ && s_httpMux && s_httpDone) {
      // R6-01（2026-10-05）：此处原有一句 xSemaphoreGive(s_httpDone)，注释称
      // 「初始为无结果」——方向反了。二值信号量 Give=「结果就绪」，导致首次
      // httpPoll 消费到幻影结果(code=0/空体)，此后请求与结果整体错位一格：
      // 24h 阶段吞掉 now 的 446B 响应 → compact 为空 → 静默保留旧缓存 →
      // 降水屏永不更新（真 24h 响应反成孤儿）。二值信号量创建后默认即为
      // 「空/无结果」，无需也不得预先 Give。
      if (xTaskCreate(httpTaskEntry, "httpTask", 6144, nullptr, 3, nullptr) == pdPASS) {
        Serial.println("[Net] http task started (core-agnostic, blocking IO isolated)");
      } else {
        Serial.println("[Net] ERR: http task create failed");
      }
    } else {
      Serial.println("[Net] ERR: http queue/semaphore alloc failed");
    }
  }
}

void NetService::loadCreds() {
  Store.getString("sys.wifi_ssid", _ssid);
  Store.getString("sys.wifi_pass", _pass);
}

// 组装凭据回退列表：[0]=NVS 用户凭据（若已配网），随后是 fw_config.h 内置默认
// （开源已清空内置默认；如需出厂回退可在此填入你自己的网络）。重复 SSID 只保留一条。
void NetService::rebuildList() {
  _listN = 0;
  if (_ssid.length()) {
    _listSsid[_listN] = _ssid; _listPass[_listN] = _pass; _listN++;
  }
  const char* defs[][2] = {
    { WIFI_DEF_SSID_1, WIFI_DEF_PASS_1 },
    { WIFI_DEF_SSID_2, WIFI_DEF_PASS_2 },
    { WIFI_DEF_SSID_3, WIFI_DEF_PASS_3 },
  };
  for (auto& d : defs) {
    if (_listN >= WIFI_CRED_MAX) break;
    bool dup = false;
    for (int i = 0; i < _listN; i++) if (_listSsid[i] == d[0]) { dup = true; break; }
    if (dup) continue;
    _listSsid[_listN] = d[0]; _listPass[_listN] = d[1]; _listN++;
  }
  if (_listIdx >= _listN) _listIdx = 0;
  _failRun = 0;
}

// 当前凭据连接失败（不在范围 / 认证失败 / 20s 超时）→ 轮换下一个凭据；
// 整轮全部失败 → 回 IDLE（外部 30s 防振荡逻辑会再次发起，从下一个起点开始）
void NetService::rotateOrStop() {
  if (_listN == 0) { _state = ST_IDLE; return; }   // 防御：无凭据不可能进入连接态
  int prev = _listIdx;
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  _suppressLostUntilMs = millis() + 4000;   // 换 AP 引发的 DISCONNECTED 事件不广播
  _listIdx = (_listIdx + 1) % _listN;
  _failRun++;
  if (_failRun >= _listN) {
    _failRun = 0;
    _state = ST_IDLE;
    Serial.printf("[Net] ERR: all %d credentials failed this round (E_NET_TIMEOUT)\n", _listN);
    return;
  }
  Serial.printf("[Net] cred #%d \"%s\" failed, rotate to #%d \"%s\"\n",
                prev + 1, _listSsid[prev].c_str(),
                _listIdx + 1, _listSsid[_listIdx].c_str());
  connectWith(_listSsid[_listIdx], _listPass[_listIdx], false);
}

void NetService::requestConnect() {
  if (_state == ST_CONNECTED) { keepAlive(); return; }
  if (_listN == 0) {
    Serial.println("[Net] ERR: no credentials (E_NET_NO_CRED)");
    return;
  }
  connectWith(_listSsid[_listIdx], _listPass[_listIdx], false);
}

void NetService::connectWith(const String& ssid, const String& pass, bool saveToNvs) {
  if (ssid.length() == 0) return;
  if (saveToNvs) {
    Store.setString("sys.wifi_ssid", ssid);
    Store.setString("sys.wifi_pass", pass);
    _ssid = ssid; _pass = pass;
    rebuildList();                     // 用户手动配的新凭据插队到首选位
    _listIdx = 0;
  }
  Serial.printf("[Net] connecting to \"%s\" ...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  _state = ST_CONNECTING;
  _lastActivityMs = millis();
}

void NetService::keepAlive() {
  _lastActivityMs = millis();
}

void NetService::tick() {
  // 延后广播 WiFi 事件（回到 loop 上下文，保证 onEvent 线程安全）
  if (_evtGotPending)  { _evtGotPending  = false; Event e; e.type = EventType::EVT_NET_GOT;  EventBus::broadcast(e); }
  if (_evtLostPending) { _evtLostPending = false; Event e; e.type = EventType::EVT_NET_LOST; EventBus::broadcast(e); }

  switch (_state) {
    case ST_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        _state = ST_CONNECTED;
        _lastActivityMs = millis();
        _failRun = 0;
        _suppressLostUntilMs = 0;
        Serial.printf("[Net] connected to \"%s\", IP=%s\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str());
        // EVT_NET_GOT 由 WiFi 事件回调广播
      } else if (WiFi.status() == WL_NO_SSID_AVAIL || WiFi.status() == WL_CONNECT_FAILED ||
                 millis() - _lastActivityMs > 20000) {
        rotateOrStop();                // 本凭据不可用 → 轮换下一个（整轮失败则回 IDLE）
      }
      break;

    case ST_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        _state = ST_IDLE;
        WiFi.mode(WIFI_OFF);
      } else if (millis() - _lastActivityMs > NET_IDLE_DISCONNECT_MS) {
        // L2：WiFi 空闲 5 分钟自动断开，进调制解调器休眠（总纲 5.4）
        Serial.println("[Net] idle 5min, disconnect (L2)");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        _state = ST_IDLE;
        // 打码区分「主动省电」与「意外掉线」：未区分时程序1 会因 L2 每 5 分钟
        // 的常规断电而永久保持 Sad 表情（看起来像一直在报错）
        Event e; e.type = EventType::EVT_NET_LOST;
        e.code = EVT_NET_LOST_CODE_POWERSAVE;
        EventBus::broadcast(e);
      }
      break;

    case ST_IDLE:
    default:
      break;
  }
}

String NetService::currentTarget() const {
  return (_listN > 0) ? _listSsid[_listIdx] : String();
}

void NetService::radioOffIfIdle() {
  if (_state == ST_IDLE) {
    WiFi.mode(WIFI_OFF);
    Serial.println("[Net] radio off (idle, L2)");
  }
}

String NetService::statusText() {
  String s;
  switch (_state) {
    case ST_CONNECTED:  s = "已连接 SSID=" + WiFi.SSID() + " IP=" + WiFi.localIP().toString(); break;
    case ST_CONNECTING:
      s = "连接中 [" + String(_listIdx + 1) + "/" + String(_listN) + "] SSID=" + currentTarget();
      break;
    default:
      s = "未连接 (";
      if (_listN == 0)        s += "无凭据";
      else if (credsSaved()) { s += "已存凭据:" + _ssid; s += "+默认x" + String(_listN - 1); }
      else                    s += "内置默认x" + String(_listN);
      s += ")";
      break;
  }
  return s;
}

// ---- 异步扫描 ----
void NetService::startScan() {
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true /*async*/, true /*show_hidden*/);
}

bool NetService::scanDone() {
  int r = WiFi.scanComplete();
  return r != WIFI_SCAN_RUNNING && r != WIFI_SCAN_FAILED;
}

// =====================================================================
// 异步 HTTP：阻塞部分跑在独立 FreeRTOS 任务（2026-10-02）
//
// 为什么必须改：原 httpGet() 是同步阻塞的（等连 15s + GET 10s + 读流 + 解压），
// 它在程序3 的 onTick 里被调用 → 主循环整段停摆 15-25 秒。
// 而按键状态机的唯一采样点就在主循环里（Manager::tick → handleButton），
// 主循环一停，按键就完全读不到 —— 实测表现为「长按切程序无效」+「天气短按不换屏」。
//
// 静态对象（队列/信号量/Job 结构）已上移至文件顶部，见 Net 定义之后。
// =====================================================================

bool NetService::httpStart(const String& url, const String& hostForDnsCheck, bool gunzip) {
  if (!s_httpQ) return false;
  if (_httpReqPending) return false;              // 已有在飞请求，丢弃本次
  if (url.length() == 0 || url.length() >= sizeof(s_httpJob.url)) return false;

  xSemaphoreTake(s_httpMux, portMAX_DELAY);
  strncpy(s_httpJob.url, url.c_str(), sizeof(s_httpJob.url) - 1);
  s_httpJob.url[sizeof(s_httpJob.url) - 1] = '\0';
  if (hostForDnsCheck.length() >= sizeof(s_httpJob.host)) {
    s_httpJob.host[0] = '\0';
  } else {
    strncpy(s_httpJob.host, hostForDnsCheck.c_str(), sizeof(s_httpJob.host) - 1);
    s_httpJob.host[sizeof(s_httpJob.host) - 1] = '\0';
  }
  s_httpJob.gunzip = gunzip;
  // R6-01：为本次请求打序号。volatile 上 ++ 触发 -Wvolatile（C++20 废弃），
  // 按 2026-10-03 编译纪律改写为显式赋值；_httpSeq 仅主循环上下文写，无竞态。
  s_httpJob.seq = _httpSeq + 1;
  _httpSeq = _httpSeq + 1;
  xSemaphoreGive(s_httpMux);

  if (xQueueSend(s_httpQ, &s_httpJob, 0) != pdTRUE) return false;
  _httpReqPending = true;
  Serial.printf("[Net] http queued: %.60s\n", url.c_str());
  return true;
}

bool NetService::httpPoll(int& outCode, String& outBody) {
  if (!_httpReqPending) return false;
  // 等结果最多 0ms：非阻塞轮询，主循环绝不能因等待而卡住
  if (xSemaphoreTake(s_httpDone, 0) != pdTRUE) return false;

  xSemaphoreTake(s_httpMux, portMAX_DELAY);
  // R6-01：结果归属校验。若拿到的是陈旧遗留结果（序号对不上），丢弃之并
  // 报告「还没好」——真正在飞请求的结果稍后由网络任务重新 Give。
  if (_httpResSeq != _httpSeq) {
    _httpResBody = "";
    _httpResReady = false;
    xSemaphoreGive(s_httpMux);
    Serial.printf("[Net] httpPoll: stale result dropped (res=%u cur=%u)\n",
                  (unsigned)_httpResSeq, (unsigned)_httpSeq);
    return false;
  }
  outCode     = _httpResCode;
  outBody     = _httpResBody;
  _httpResBody = "";
  _httpResReady = false;
  xSemaphoreGive(s_httpMux);

  _httpReqPending = false;
  return true;
}

void NetService::httpTaskEntry(void* arg) {
  (void)arg;
  HttpJob job;
  for (;;) {
    if (xQueueReceive(s_httpQ, &job, portMAX_DELAY) != pdTRUE) continue;

    String body;
    int code = Net.httpGetBlocking(String(job.url), String(job.host), body, job.gunzip);

    xSemaphoreTake(s_httpMux, portMAX_DELAY);
    Net._httpResCode   = code;
    Net._httpResBody   = body;
    Net._httpResSeq    = job.seq;      // R6-01：结果带回请求序号
    Net._httpResReady  = true;
    xSemaphoreGive(s_httpMux);

    xSemaphoreGive(s_httpDone);      // 唤醒主循环取结果
    Serial.printf("[Net] http done: code=%d len=%u\n", code, (unsigned)body.length());
  }
}

int NetService::httpGetBlocking(const String& url, const String& hostForDnsCheck,
                                String& outBody, bool gunzip) {
  outBody = "";

  // 已连接是调用方（主循环）的前置条件：连接状态机不在本任务里跑。
  // 未连接直接快速失败，绝不在此阻塞等待（那会把「等连」又塞回主循环）。
  if (_state != ST_CONNECTED) {
    Serial.println("[Net] http blocked: not connected");
    return -101;    // E_NET_TIMEOUT
  }

  // Host 预检（2026-10-02 上板实测教训）：
  // 内置 Host 若区域码猜错，域名根本不存在，日志里看不出是「域名错」还是「网络不通」。
  // ⚠️ WiFi.hostByName() 对不存在的域名仍返回 true，但 IP 为 0.0.0.0 → 必须同时校验 IP 非 0。
  // 本预检同样阻塞，故也在网络任务内执行。
  if (hostForDnsCheck.length() > 0) {
    IPAddress ip;
    bool dnsOk = WiFi.hostByName(hostForDnsCheck.c_str(), ip) && ip != IPAddress(0, 0, 0, 0);
    if (!dnsOk) {
      Serial.printf("[Net] DNS 解析失败: %s\n", hostForDnsCheck.c_str());
      return E_WX_HOST;
    }
    Serial.printf("[Net] host OK %s -> %s\n", hostForDnsCheck.c_str(), ip.toString().c_str());
  }

  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(WX_HTTP_TIMEOUT_MS);
  // 2026-10-02 上板实测修正：本工具链（esp32:esp32 3.3.12-cn）**不带 zlib**，
  // 故自带 inflate（fw_inflate.cpp）承担解压。
  // 实测和风天气**无视 Accept-Encoding: identity，仍强制返回 GZIP**，
  // 因此直接按正常方式请求（服务器给什么解什么），由魔数嗅探决定是否解压。
  http.addHeader("Accept-Encoding", gunzip ? "gzip" : "identity");
  http.setUserAgent("TaskManagerFirmware/" FW_VERSION);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[Net] httpGet ERR: %d %s\n", code, url.c_str());
    http.end();
    return code;
  }

  // 分块读取响应体（含 3s 无进展超时，防服务器半途卡死拖满连接超时）
  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  const size_t CH = 1024;
  uint8_t buf[CH];
  int total = 0;
  uint32_t lastDataMs = millis();
  while (http.connected() && (len < 0 || total < len)) {
    size_t avail = stream->available();
    if (avail) {
      int c = stream->readBytes(buf, min(avail, CH));
      outBody.concat((const char*)buf, (unsigned int)c);
      total += c;
      lastDataMs = millis();
    } else if (millis() - lastDataMs > 3000) {
      Serial.println("[Net] httpGet WARN: read stalled 3s, abort");
      break;
    } else {
      delay(2);
    }
  }
  http.end();

  if (gunzip) {
    // 内容嗅探兜底：实测和风天气无视 Accept-Encoding: identity 仍强制返回 GZIP，
    // 故必须真的能解压。一律走 gunzipBytes()（有 zlib 用 zlib，否则用自带 inflate）。
    bool looksGz = (outBody.length() >= 2 &&
                    (uint8_t)outBody[0] == 0x1F && (uint8_t)outBody[1] == 0x8B);
    if (looksGz) {
      String plain;
      if (!gunzipBytes((const uint8_t*)outBody.c_str(), outBody.length(), plain)) {
        Serial.printf("[Net] httpGet ERR: gunzip failed (E_NET_GZIP), len=%u\n",
                      (unsigned)outBody.length());
        return -102;   // E_NET_GZIP
      }
      outBody = plain;
    }
  }
  Serial.printf("[Net] httpGet OK: %d, %u bytes %s\n", code, (unsigned)outBody.length(), gunzip ? "(gz)" : "");
  return code;
}

bool NetService::gunzipBytes(const uint8_t* src, size_t srclen, String& out) {
#if FW_HAS_ZLIB
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;   // 16 = gzip wrapper
  static uint8_t obuf[2048];
  zs.next_in = const_cast<Bytef*>(src);
  zs.avail_in = (uInt)srclen;
  out = "";
  bool ok = false;
  for (;;) {
    zs.next_out = obuf;
    zs.avail_out = sizeof(obuf);
    int ret = inflate(&zs, Z_NO_FLUSH);
    size_t got = sizeof(obuf) - zs.avail_out;
    if (got) out.concat((const char*)obuf, (unsigned int)got);
    if (ret == Z_STREAM_END) { ok = true; break; }
    if (ret != Z_OK) break;
    if (got == 0) break;   // 无进展，防死循环
  }
  inflateEnd(&zs);
  return ok;
#else
  // 本工具链无 zlib（2026-10-02 实测：esp32:esp32 3.3.12-cn 不含 zlib.h/libz.a），
  // 改用自带 inflate（fw_inflate.cpp）。已对照 zlib 验证 8/8，CRC32 亦校验。
  // 上限 16 KB：QWeather now ≈311 B、24h ≈859 B，16 KB 足够且防损坏数据撑爆 RAM。
  Inflate::setOutputLimit(16384);
  bool ok = Inflate::gunzip(src, srclen, out);
  if (!ok) Serial.printf("[Net] inflate 失败: %s (输入 %u B)\n",
                         Inflate::lastError(), (unsigned)srclen);
  return ok;
#endif
}
