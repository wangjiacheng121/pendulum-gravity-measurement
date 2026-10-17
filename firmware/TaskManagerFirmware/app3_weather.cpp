// =====================================================================
// app3_weather.cpp —— 程序3「实时天气」实现（分册 03）
// JSON 解析采用轻量字段提取（QWeather 响应字段均为扁平字符串，
// 不引入第三方 JSON 库，控制 Flash 体积）
// =====================================================================
#include "app3_weather.h"
#include "fw_config.h"
#include "fw_store.h"
#include "fw_display.h"
#include "fw_time.h"
#include "fw_manager.h"
#include "fw_ble.h"
#include "fw_net.h"

// 真实运行的实例在 TaskManagerFirmware.ino 中定义（静态对象），此处不另建副本。

// ===================== 轻量 JSON 字段提取 =====================
// 取 "key":"value" 或 "key":123 的 value（QWeather 字符串为主）
static bool jsonVal(const String& j, const char* key, String& out) {
  String pat = String("\"") + key + "\"";
  int p = j.indexOf(pat);
  if (p < 0) return false;
  p = j.indexOf(':', p + pat.length());
  if (p < 0) return false;
  p++;
  while (p < (int)j.length() && (j[p] == ' ' || j[p] == '\t')) p++;
  if (p >= (int)j.length()) return false;
  if (j[p] == '"') {
    int e = j.indexOf('"', p + 1);
    if (e < 0) return false;
    out = j.substring(p + 1, e);
  } else {
    int e = p;
    while (e < (int)j.length() && j[e] != ',' && j[e] != '}' && j[e] != ']') e++;
    out = j.substring(p, e);
    out.trim();
  }
  return out.length() > 0;
}

// 24h 原始 JSON → 紧凑存储 "fxTime(16字符)|temp|pop;…"
// 原因：NVS 单条变长值受页大小限制（约 4KB），24h 原始 JSON 约 5-6KB 可能静默写入失败
static void compactHourly(const String& j, String& out) {
  out = "";
  int pos = 0;
  for (int i = 0; i < 24; i++) {
    int p = j.indexOf("\"fxTime\"", pos);
    if (p < 0) break;
    int e = j.indexOf('}', p);
    if (e < 0) e = j.length();
    String obj = j.substring(p, e);
    String fx, tS, pop;
    if (jsonVal(obj, "fxTime", fx) && fx.length() >= 16) {
      if (out.length()) out += ";";
      out += fx.substring(0, 16);                     // "2026-09-30T19:00"
      out += "|";
      out += jsonVal(obj, "temp", tS) ? tS : "--";
      out += "|";
      out += jsonVal(obj, "pop", pop) ? pop : "0";
    }
    pos = e;
  }
}

// ===================== 生命周期 =====================

void WeatherApp::onCreate() {
  _page = (int)Store.getInt("app3.wx_page", 1);
  if (_page < 1 || _page > 4) _page = 1;
  _lastErr = 0;
  _pending = false;
  Ble.registerMenu("4", "天气", this);
  Serial.printf("[Wx] onCreate page=%d\n", _page);
}

void WeatherApp::onShow() {
  // 上次停留屏恢复（app3.wx_page）
  if (due() || _lastErr != 0) schedule();   // 到期/有错 → 到点即拉
  _needDraw = true;
}

void WeatherApp::onHide() {
  // 切走即终止本轮拉取状态机：非活跃程序不得继续发起网络请求（分册03策略）。
  // 注：在飞的那个 HTTP 请求会由网络任务跑完并丢弃结果（主循环不再 poll），
  // 这是有意为之——强行中断网络任务比重跑一次更危险。
  _pending = false;
  _fx = FX_IDLE;
  _fxActive = false;
}

void WeatherApp::onButton() {
  _page = _page % 4 + 1;                    // 四屏固定顺序循环
  Store.setInt("app3.wx_page", _page);
  _needDraw = true;
  draw();
}

void WeatherApp::onEvent(const Event& evt) {
  if (evt.type == EventType::EVT_NET_GOT) {
    // 断网恢复 → 立即补一次（分册03第3节；仅活跃时执行，非活跃零阻塞）
    if (Manager.activeId() == AppId::WEATHER) schedule();
  }
}

void WeatherApp::schedule() {
  _pending = true;
  _triedOnce = false;
  _retryAtMs = 0;                           // 下个 tick 立即尝试
  _fx = FX_WAIT_NET;
  _fxActive = false;
}

bool WeatherApp::due() {
  uint32_t ts = (uint32_t)Store.getInt("app3.wx_now_ts", 0);
  if (ts == 0) return true;                 // 无缓存
  uint32_t nowE = Time.epoch();
  if (nowE == 0) return true;               // 时间未同步，视为到期
  // R4-01：改用秒比较，避免 (nowE-ts)*1000UL 在断网 >49.7 天时 uint32 溢出，
  // 误判「未到期」→ 陈旧数据永不刷新（张家口项目几乎不触发，但务必正确）
  return (nowE - ts) >= (WX_PERIOD_MS / 1000UL);
}

// ===================== 异步拉取状态机（2026-10-02） =====================
// 关键约束：onTick 单次必须 «50ms 铁律，且主循环绝不能被网络阻塞 ——
// 按键状态机的采样点就在主循环里，一阻塞按键就失灵（本次故障根因）。
// 因此这里只做「投递请求 / 查结果 / 推进阶段」，重活全在 fw_net 的网络任务里。

String WeatherApp::resolveHost() {
  String host, key, lonS, latS;
  Store.getString("sys.wx_host", host);
  if (host.length() == 0) host = WX_HOST_DEFAULT;
  if (key.length()  == 0) key  = WX_KEY_DEFAULT;   // 仅用于判定，值本身在 buildUrl 取
  if (lonS.length() == 0) lonS = LOC_LON_DEFAULT;
  if (latS.length() == 0) latS = LOC_LAT_DEFAULT;
  if (host.length() == 0) return "";
  if (!host.startsWith("http")) host = "https://" + host;
  while (host.endsWith("/")) host.remove(host.length() - 1);
  return host;
}

String WeatherApp::buildUrl(const char* path) {
  String host, key, lonS, latS;
  Store.getString("sys.wx_host", host);
  Store.getString("sys.wx_key", key);
  Store.getString("sys.loc_lon", lonS);
  Store.getString("sys.loc_lat", latS);
  // 兜底：NVS 被清空/首次烧录未写入时，回落到固件内置凭据（2026-10-02 用户要求
  // Key 写入固件）。NVS 有值时仍以 NVS 为准，便于 [4.4] 换账号而无需重刷。
  if (host.length() == 0) host = WX_HOST_DEFAULT;
  if (key.length()  == 0) key  = WX_KEY_DEFAULT;
  if (lonS.length() == 0) lonS = LOC_LON_DEFAULT;
  if (latS.length() == 0) latS = LOC_LAT_DEFAULT;
  if (host.length() == 0 || key.length() == 0) return "";
  if (!host.startsWith("http")) host = "https://" + host;
  while (host.endsWith("/")) host.remove(host.length() - 1);

  String loc = lonS + "," + latS;           // QWeather：经度在前、纬度在后！
  return host + path + "?location=" + loc + "&key=" + key + "&lang=zh&unit=m";
}

bool WeatherApp::startStage(FxStage st) {
  const char* path = (st == FX_NOW) ? "/v7/weather/now" : "/v7/weather/24h";
  String url = buildUrl(path);
  if (url.length() == 0) {
    _lastErr = E_NET_NO_CRED;
    Serial.println("[Wx] ERR: wx_host/wx_key not configured (E_NET_NO_CRED)");
    updateFailed();
    return false;
  }
  if (!Net.httpStart(url, resolveHost(), true)) {
    return false;                            // 网络忙（上一请求未取回），下 tick 重试
  }
  _fx = st;
  return true;
}

void WeatherApp::advanceFx() {
  if (!_pending) return;
  uint32_t now = millis();

  // 重试门限未到 → 本 tick 不做事
  if ((int32_t)(now - _retryAtMs) < 0) return;

  switch (_fx) {
    case FX_IDLE:
      _fx = FX_WAIT_NET;
      _fxActive = false;
      break;

    case FX_WAIT_NET: {
      if (Net.connected()) {
        Net.keepAlive();
        if (startStage(FX_NOW)) _fxActive = true;
        break;
      }
      // 惰性自动重连（30s 间隔防错误凭据振荡）
      if (!Net.connecting() && Net.hasAnyCred() &&
          (int32_t)(now - _lastConnReqMs) >= 0 &&
          (now - _lastConnReqMs) >= 30000) {
        _lastConnReqMs = now;
        Net.requestConnect();
      }
      break;
    }

    case FX_NOW:
    case FX_24H: {
      if (!Net.httpBusy()) break;            // 结果还没回来，下 tick 再看
      int code = 0;
      String body;
      if (!Net.httpPoll(code, body)) break;  // 非阻塞取结果

      bool isNow = (_fx == FX_NOW);
      if (code != 200) {
        _lastErr = (code == E_WX_HOST) ? E_WX_HOST : code;
        updateFailed();
        break;
      }
      // API 业务码
      String apiCode, probe;
      bool apiOk = jsonVal(body, "code", apiCode) && apiCode == "200";
      if (!apiOk) {
        _lastErr = E_WX_API;
        updateFailed();
        break;
      }
      if (isNow) {
        if (!jsonVal(body, "temp", probe)) {  // now 接口必须有 temp
          _lastErr = E_WX_PARSE;
          updateFailed();
          break;
        }
        Store.setString("app3.wx_now", body);
        Store.setInt("app3.wx_now_ts", (int32_t)Time.epoch());
        // now 成功 → 接着拉 24h（逐段推进，仍不阻塞）
        if (startStage(FX_24H)) break;
        // 24h 发起失败（网络忙）→ 保持 FX_NOW 已完成，等下一 tick 重入
        _fx = FX_NOW;
        break;
      }
      // 24h 完成（失败也允许，只保留 now）
      String compact;
      compactHourly(body, compact);
      if (compact.length() > 0) {
        Store.setString("app3.wx_hourly", compact);   // 紧凑格式（~600B，NVS 安全）
        Store.setInt("app3.wx_hourly_ts", (int32_t)Time.epoch());
      } else {
        Serial.println("[Wx] WARN: hourly compact empty, keep old cache");
      }
      _lastErr = 0;
      _pending = false;
      _fx = FX_IDLE;
      _fxActive = false;
      {
        Event e; e.type = EventType::EVT_WX_UPDATE_OK;
        EventBus::broadcast(e);                 // 桌宠 wxok 表情联动
      }
      Serial.println("[Wx] update OK");
      _needDraw = true;
      break;
    }
  }
}

void WeatherApp::onTick(float dt) {
  (void)dt;
  uint32_t now = millis();

  advanceFx();   // 非阻塞推进拉取状态机

  // 心跳刷新角标（30s 一次，避免频繁整帧重绘）
  if (_needDraw || (int32_t)(now - _lastHeartbeat) >= 30000) {
    _lastHeartbeat = now;
    _needDraw = false;
    draw();
  }
}

// ===================== 拉取与缓存（分册03第2、3节） =====================
// 旧的阻塞式 doUpdate() 已于 2026-10-02 删除，逻辑迁入上方 advanceFx() 状态机。

void WeatherApp::onBleAbort() {
  _bs = BS_NONE;      // 不清会让重连后再发 [4] 直接掉进上一次的输入态
}

void WeatherApp::updateFailed() {
  Serial.printf("[Wx] ERR: update failed code=%d\n", _lastErr);
  // 失败绝不切屏、绝不打断当前显示（分册03第1节）
  Event e; e.type = EventType::EVT_WX_UPDATE_FAIL; e.code = _lastErr;
  EventBus::broadcast(e);
  // 状态机必须回到「等连接」阶段重走，否则重试会停在已失败的阶段上，
  // 变成永久卡死（异步化后 _fx 记录进度，失败时须显式复位）
  _fx = FX_WAIT_NET;
  _fxActive = false;
  if (!_triedOnce) {
    _triedOnce = true;
    _retryAtMs = millis() + WX_RETRY_DELAY_MS;   // 30s 后重试 1 次
  } else {
    _pending = false;
    _fx = FX_IDLE;
  }
  _needDraw = true;
}

// ===================== 绘制（分册03第4节四屏） =====================

void WeatherApp::drawIcon(int x, int y, const String& text) {
  // 简笔点阵图标：晴/云/雨/雪/雾 5 类（含雷归雨、霾归雾）
  if (text.indexOf("雪") >= 0) {                       // 雪：云 + 三点
    Display.drawDisc(x + 12, y + 8, 6); Display.drawDisc(x + 4, y + 10, 4);
    Display.drawBox(x + 2, y + 10, 20, 5);
    Display.drawDisc(x + 6, y + 20, 1); Display.drawDisc(x + 12, y + 21, 1); Display.drawDisc(x + 18, y + 20, 1);
    return;
  }
  if (text.indexOf("雨") >= 0 || text.indexOf("雷") >= 0) {  // 雨：云 + 三斜线
    Display.drawDisc(x + 12, y + 6, 6); Display.drawDisc(x + 4, y + 8, 4);
    Display.drawBox(x + 2, y + 8, 20, 5);
    Display.drawLine(x + 6, y + 16, x + 4, y + 22);
    Display.drawLine(x + 13, y + 16, x + 11, y + 22);
    Display.drawLine(x + 20, y + 16, x + 18, y + 22);
    return;
  }
  if (text.indexOf("雾") >= 0 || text.indexOf("霾") >= 0 || text.indexOf("沙") >= 0 || text.indexOf("尘") >= 0) {
    Display.drawLine(x + 2, y + 6, x + 22, y + 6);     // 雾：四横线
    Display.drawLine(x + 5, y + 11, x + 25, y + 11);
    Display.drawLine(x + 2, y + 16, x + 22, y + 16);
    Display.drawLine(x + 5, y + 21, x + 25, y + 21);
    return;
  }
  if (text.indexOf("云") >= 0 || text.indexOf("阴") >= 0) {  // 云
    Display.drawDisc(x + 12, y + 9, 7); Display.drawDisc(x + 4, y + 12, 5);
    Display.drawDisc(x + 19, y + 12, 5);
    Display.drawBox(x + 2, y + 12, 22, 5);
    return;
  }
  // 晴（默认）：圆 + 8 条光线
  Display.drawDisc(x + 12, y + 12, 6);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.785398f;   // PI/8*2
    int x1 = x + 12 + (int)(10 * cos(a)), y1 = y + 12 + (int)(10 * sin(a));
    int x2 = x + 12 + (int)(13 * cos(a)), y2 = y + 12 + (int)(13 * sin(a));
    Display.drawLine(x1, y1, x2, y2);
  }
}

void WeatherApp::draw() {
  if (!Display.present()) return;
  Display.clearBuffer();

  String nowJ, hourlyJ;
  uint32_t nowTs = (uint32_t)Store.getInt("app3.wx_now_ts", 0);
  // 原代码另外读取了 wx_hourly_ts 但从未使用（每次重绘白白多一次 NVS 读）。
  // 逐时缓存年龄不在当前 4 屏版式内，先移除；日后若要在第 4 屏加角标再恢复
  bool hasNow = Store.getString("app3.wx_now", nowJ) && nowJ.length() > 0;
  bool hasHl  = Store.getString("app3.wx_hourly", hourlyJ) && hourlyJ.length() > 0;

  if (!hasNow) {
    // 首次无任何缓存：等待页（分册03第4节）
    Display.setFont(DisplayService::FONT_CN12);
    Display.centerUtf8(28, "等待天气数据…");
    Display.centerUtf8(46, "（配网后自动获取）");
    Display.setFont(DisplayService::FONT_TINY);
    Display.drawStr(2, 63, "1/4");
    Display.commit();
    return;
  }

  // now 字段
  String temp, feels, text, windDir, windScale, windSpeed, humidity;
  jsonVal(nowJ, "temp", temp);
  jsonVal(nowJ, "feelsLike", feels);
  jsonVal(nowJ, "text", text);
  jsonVal(nowJ, "windDir", windDir);
  jsonVal(nowJ, "windScale", windScale);
  jsonVal(nowJ, "windSpeed", windSpeed);
  jsonVal(nowJ, "humidity", humidity);

  switch (_page) {
    case 1: {   // ① 气温
      Display.setFont(DisplayService::FONT_NUM);
      Display.drawStr(0, 34, temp.c_str());
      int w = Display.getStrWidth(temp);
      Display.setFont(DisplayService::FONT_CN16);
      Display.drawUtf8(w + 4, 34, "°C");
      Display.setFont(DisplayService::FONT_CN12);
      Display.drawUtf8(0, 54, "体感 " + feels + "°C  湿度 " + humidity + "%");
      break;
    }
    case 2: {   // ② 天气 + 图标
      drawIcon(96, 4, text);
      Display.setFont(DisplayService::FONT_CN16);
      Display.drawUtf8(0, 24, text);
      // 今日 最高/最低（紧凑缓存按日期分组，取第一条所属日期）
      String hi = "--", lo = "--";
      if (hasHl) {
        double tmax = -999, tmin = 999;
        String firstDate = "";
        int start = 0;
        while (start < (int)hourlyJ.length()) {
          int e = hourlyJ.indexOf(';', start);
          if (e < 0) e = hourlyJ.length();
          String ent = hourlyJ.substring(start, e);
          start = e + 1;
          int b1 = ent.indexOf('|');
          int b2 = ent.indexOf('|', b1 + 1);
          if (b1 <= 0 || b2 < 0) continue;
          String fx = ent.substring(0, b1);
          if (firstDate.length() == 0) firstDate = fx.substring(0, 10);
          if (!fx.startsWith(firstDate)) break;    // 只统计第一天
          // R4-02：温度缺失时紧凑缓存存 "--"，toDouble() 得 0，会被当成 0°C
          // 计入今日极值（显示「今日 0/0°C」）。跳过缺失条目，避免污染极值。
          String tStr = ent.substring(b1 + 1, b2);
          if (tStr == "--") continue;
          double t = tStr.toDouble();
          if (t > tmax) tmax = t;
          if (t < tmin) tmin = t;
        }
        if (tmax > -900) hi = String(tmax, 0);
        if (tmin <  900) lo = String(tmin, 0);
      }
      Display.setFont(DisplayService::FONT_CN12);
      Display.drawUtf8(0, 48, "今日 " + hi + "/" + lo + "°C");
      break;
    }
    case 3: {   // ③ 风速
      Display.setFont(DisplayService::FONT_NUM);
      Display.drawStr(0, 34, windSpeed.c_str());
      int w = Display.getStrWidth(windSpeed);
      Display.setFont(DisplayService::FONT_CN16);
      Display.drawUtf8(w + 6, 34, "km/h");
      Display.setFont(DisplayService::FONT_CN12);
      Display.drawUtf8(0, 54, windDir + " " + windScale + "级");
      break;
    }
    case 4: {   // ④ 未来 6h 降水点阵
      // 6 竖条：pop 0-100% 映射条高 0-34px（数据来自紧凑缓存前 6 条）
      int pops[6]; String times[6];
      for (int i = 0; i < 6; i++) { pops[i] = 0; times[i] = "--:--"; }
      String rainFrom = "";
      if (hasHl) {
        int start = 0;
        for (int i = 0; i < 6; i++) {
          int e = hourlyJ.indexOf(';', start);
          if (e < 0) e = hourlyJ.length();
          String ent = hourlyJ.substring(start, e);
          start = e + 1;
          int b1 = ent.indexOf('|');
          int b2 = ent.indexOf('|', b1 + 1);
          if (b1 < 16 || b2 < 0) break;              // fxTime 段须满 16 字符
          times[i] = ent.substring(11, 16);          // "HH:MM"
          pops[i] = (int)ent.substring(b2 + 1).toDouble();
          if (rainFrom.length() == 0 && pops[i] >= 30) rainFrom = times[i];   // pop≥30%
        }
      }
      for (int i = 0; i < 6; i++) {
        int h = pops[i] * 34 / 100;
        int x = 6 + i * 21;
        if (h > 0) Display.drawBox(x, 44 - h, 14, h);
        Display.drawBox(x, 46, 14, 1);                  // 底线
        Display.setFont(DisplayService::FONT_TINY);
        Display.drawStr(x, 55, times[i].substring(0, 2).c_str());   // 小时
      }
      Display.setFont(DisplayService::FONT_CN12);
      Display.drawUtf8(0, 63, rainFrom.length() ? ("降水约从 " + rainFrom + " 开始")
                                                : "未来6h无明显降水");
      break;
    }
  }

  // 角标：数据年龄 + 屏序 + 错误旗标（分册03第3节：>24h 追加 !）
  Display.setFont(DisplayService::FONT_CN12);
  if (nowTs > 0) {
    uint32_t nowE = Time.epoch();
    String age = Time.hhmmOf(nowTs) + "数据";
    if (nowE > 0 && nowTs > 0 && (nowE - nowTs) > 86400) age += "!";
    if (_lastErr != 0) age += " E";
    Display.rightUtf8(12, age);
  }
  Display.setFont(DisplayService::FONT_TINY);
  Display.drawStr(2, 63, (String(_page) + "/4").c_str());

  Display.commit();
}

// ===================== BLE 菜单（分册03第5节） =====================

bool WeatherApp::onBleMsg(const String& line, String& reply) {
  // ---- 菜单入口 ----
  if (_bs == BS_NONE) {
    if (line != "4") return false;
    _bs = BS_ROOT;
    Ble.replyINFO("=== 天气 ===");
    Ble.replyINFO("[4.1] 查看状态");
    Ble.replyINFO("[4.2] 修改坐标");
    Ble.replyINFO("[4.3] 立即更新");
    Ble.replyINFO("[4.4] 配置 Key/Host");
    Ble.replyINFO("[4.5] 清除凭据改用内置");
    Ble.replyINFO("q 退出");
    Ble.beginSession(this);
    return true;
  }

  if (line == "q") { reply = "INFO: 退出"; _bs = BS_NONE; Ble.endSession(); return true; }

  switch (_bs) {
    case BS_ROOT:
      if (line == "4.1" || line == "1") {
        String key, host, lonS, latS;
        Store.getString("sys.wx_key", key);
        Store.getString("sys.wx_host", host);
        Store.getString("sys.loc_lon", lonS);
        Store.getString("sys.loc_lat", latS);
        uint32_t ts = (uint32_t)Store.getInt("app3.wx_now_ts", 0);
        uint32_t hts = (uint32_t)Store.getInt("app3.wx_hourly_ts", 0);
        Ble.replyINFO(String("Key=") + (key.length() ? "已配置" : "未配置(用内置)") +
                      " Host=" + (host.length() ? "已配置" : "未配置(用内置)"));
        Ble.replyINFO(String("逐时缓存=") + (hts ? Time.hhmmOf(hts) : String("无")) +
                      "（降水屏数据源；与「上次更新」应同为近期）");
        reply = "INFO: 坐标=" + lonS + "," + latS +
                " 上次更新=" + Time.hhmmOf(ts) +
                " 错误=" + (_lastErr ? String(_lastErr) : "无");
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      if (line == "4.2" || line == "2") {
        Ble.replyINFO("输入「经度,纬度」（如 114.88,40.77）：");
        _bs = BS_COORD;
        return true;
      }
      if (line == "4.3" || line == "3") {
        schedule();
        reply = "OK: 已提交";
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      if (line == "4.4" || line == "4") {
        Ble.replyINFO("步骤1/2：输入 API Host（如 xxxxx.re.qweatherapi.com）：");
        _bs = BS_HOST;
        return true;
      }
      // [4.5] 清除 NVS 凭据 → 回落到固件内置值。
      // 用途：① 换固件版本后内置 Host 更新，需刷新 NVS 里的旧值；
      //       ② 凭据写错了要回退。NVS 有值时优先于内置，故必须显式清除。
      if (line == "4.5" || line == "5") {
        Store.removeKey("sys.wx_host");
        Store.removeKey("sys.wx_key");
        schedule();
        Ble.replyINFO("OK: 已清除 NVS 凭据，将使用固件内置值：");
        reply = "INFO: 内置 Host=" + String(WX_HOST_DEFAULT) + "；正在拉取";
        _bs = BS_NONE; Ble.endSession();
        return true;
      }
      Ble.replyERR("输入 4.1/4.2/4.3/4.4 或 q");
      return true;

    case BS_COORD: {
      int c = line.indexOf(',');
      if (c <= 0) { Ble.replyERR("格式：经度,纬度"); return true; }
      double lon = line.substring(0, c).toDouble();
      double lat = line.substring(c + 1).toDouble();
      if (lon < 73.0 || lon > 135.0 || lat < 3.0 || lat > 54.0) {
        Ble.replyERR("超出中国范围（经度73-135，纬度3-54）");
        return true;
      }
      Store.setString("sys.loc_lon", String(lon, 4));
      Store.setString("sys.loc_lat", String(lat, 4));
      schedule();                            // 立即触发一次拉取（分册03第5节）
      reply = "OK: 坐标已更新，即将拉取";
      _bs = BS_NONE; Ble.endSession();
      return true;
    }

    case BS_HOST:
      if (line.length() < 5) { Ble.replyERR("Host 无效"); return true; }
      _tmpHost = line;
      Ble.replyINFO("步骤2/2：输入 API Key：");
      _bs = BS_KEY;
      return true;

    case BS_KEY:
      if (line.length() < 8) { Ble.replyERR("Key 无效"); return true; }
      Store.setString("sys.wx_host", _tmpHost);
      Store.setString("sys.wx_key", line);
      schedule();                            // 配好凭据立即拉取（原版只提示"可用 4.3"，用户会误以为要手动再发一次）
      Ble.replyINFO("OK: Host=" + _tmpHost + " Key=****（已打码）");
      reply = "INFO: 凭据已保存，程序3活跃时立即拉取；失败可用 [4.1] 查错误码";
      _bs = BS_NONE; Ble.endSession();
      return true;

    default:
      _bs = BS_NONE; Ble.endSession();
      reply = "INFO: 已退出";
      return true;
  }
}
