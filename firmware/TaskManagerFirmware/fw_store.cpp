// =====================================================================
// fw_store.cpp —— 存储服务实现（NVS / Preferences）
// =====================================================================
#include "fw_store.h"
#include "fw_config.h"
#include <Preferences.h>

StoreService Store;

// ---------- 命名空间校验与拆分（总纲第 7 节：仅允许 sys./app1./app2./app3.） ----------
bool StoreService::split(const String& dottedKey, const char*& ns, String& key) {
  int dot = dottedKey.indexOf('.');
  if (dot <= 0) return false;
  String prefix = dottedKey.substring(0, dot);
  key = dottedKey.substring(dot + 1);
  if (key.length() == 0) return false;
  if      (prefix == "sys")  ns = "sys";
  else if (prefix == "app1") ns = "app1";
  else if (prefix == "app2") ns = "app2";
  else if (prefix == "app3") ns = "app3";
  else return false;
  return true;
}

void StoreService::begin() {
  // 出厂默认值（仅首次写入；不覆盖用户/配网数据）
  if (!hasKey("sys.screen_saver_timeout")) setInt("sys.screen_saver_timeout", SCREEN_SAVER_DEFAULT_S);
  if (!hasKey("sys.loc_lon")) setString("sys.loc_lon", LOC_LON_DEFAULT);
  if (!hasKey("sys.loc_lat")) setString("sys.loc_lat", LOC_LAT_DEFAULT);
  // 天气凭据出厂即写入内置值（2026-10-02 用户要求：Key 写入固件，开机就能拉天气，
  // 不再必须先经 BLE 4.4 配一次）。仅当键不存在时写，不覆盖用户后续 [4.4] 的修改。
  if (!hasKey("sys.wx_host")) setString("sys.wx_host", WX_HOST_DEFAULT);
  if (!hasKey("sys.wx_key"))  setString("sys.wx_key",  WX_KEY_DEFAULT);
  // sys.wifi_ssid / wifi_pass 出厂留空（总纲第 7 节）。
  // WiFi 未配置时由 Net 使用 fw_config.h 内置默认回退列表（2026-09-30 用户要求内置）。
  // ⚠️ 天气 Key 与 WiFi 默认凭据均为个人私有凭据，开源前务必清空 fw_config.h。
}

bool StoreService::getString(const String& dottedKey, String& out) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return false;
  Preferences prefs;
  if (!prefs.begin(ns, true)) return false;
  out = prefs.getString(key.c_str(), "");
  prefs.end();
  return true;
}

void StoreService::setString(const String& dottedKey, const String& val) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return;
  Preferences prefs;
  if (!prefs.begin(ns, false)) return;
  prefs.putString(key.c_str(), val);
  prefs.end();
}

bool StoreService::hasKey(const String& dottedKey) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return false;
  Preferences prefs;
  if (!prefs.begin(ns, true)) return false;
  bool r = prefs.isKey(key.c_str());
  prefs.end();
  return r;
}

void StoreService::removeKey(const String& dottedKey) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return;
  Preferences prefs;
  if (!prefs.begin(ns, false)) return;
  prefs.remove(key.c_str());
  prefs.end();
}

int32_t StoreService::getInt(const String& dottedKey, int32_t def) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return def;
  Preferences prefs;
  if (!prefs.begin(ns, true)) return def;
  int32_t v = prefs.getInt(key.c_str(), def);
  prefs.end();
  return v;
}

void StoreService::setInt(const String& dottedKey, int32_t val) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return;
  Preferences prefs;
  if (!prefs.begin(ns, false)) return;
  prefs.putInt(key.c_str(), val);
  prefs.end();
}

double StoreService::getDouble(const String& dottedKey, double def) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return def;
  Preferences prefs;
  if (!prefs.begin(ns, true)) return def;
  double v = prefs.getDouble(key.c_str(), def);
  prefs.end();
  return v;
}

void StoreService::setDouble(const String& dottedKey, double val) {
  const char* ns; String key;
  if (!split(dottedKey, ns, key)) return;
  Preferences prefs;
  if (!prefs.begin(ns, false)) return;
  prefs.putDouble(key.c_str(), val);
  prefs.end();
}

// ===================== HistoryStore =====================

HistoryStore::HistoryStore(const char* ns, int cap) : _ns(ns), _cap(cap) {}

void HistoryStore::begin() {
  // 游标键不存在时初始化为 0
  Preferences prefs;
  if (prefs.begin(_ns.c_str(), true)) {
    bool hasHead = prefs.isKey("hist_head");
    prefs.end();
    if (!hasHead) {
      if (prefs.begin(_ns.c_str(), false)) {
        prefs.putInt("hist_head", 0);
        prefs.putInt("hist_cnt", 0);
        prefs.end();
      }
    }
  }
}

void HistoryStore::append(const String& line) {
  if (line.length() > 200) return;                 // 单条过长拒收，防止 NVS 膨胀
  Preferences prefs;
  if (!prefs.begin(_ns.c_str(), false)) return;
  int head = prefs.getInt("hist_head", 0);
  int cnt  = prefs.getInt("hist_cnt", 0);
  char key[32];
  snprintf(key, sizeof(key), "hist_%02d", head % _cap);
  prefs.putString(key, line);
  prefs.putInt("hist_head", (head + 1) % (_cap * 1000)); // 防溢出，取模仅用于游标收敛
  prefs.putInt("hist_cnt", cnt < _cap ? cnt + 1 : cnt);
  prefs.end();
}

int HistoryStore::count() {
  Preferences prefs;
  if (!prefs.begin(_ns.c_str(), true)) return 0;
  int cnt = prefs.getInt("hist_cnt", 0);
  prefs.end();
  return cnt;
}

void HistoryStore::forEach(const std::function<bool(int, const String&)>& cb) {
  Preferences prefs;
  if (!prefs.begin(_ns.c_str(), true)) return;
  int head = prefs.getInt("hist_head", 0);
  int cnt  = prefs.getInt("hist_cnt", 0);
  int oldest = (cnt < _cap) ? 0 : head;            // 未满：从 0 开始；已满：head 即最旧
  for (int i = 0; i < cnt; i++) {
    int slot = (oldest + i) % _cap;
    char key[32];
    snprintf(key, sizeof(key), "hist_%02d", slot);
    String line = prefs.getString(key, "");
    if (line.length() == 0) continue;
    if (!cb(i, line)) break;
  }
  prefs.end();
}

bool HistoryStore::getAt(int idx, String& out) {
  if (idx < 0) return false;
  int cnt = count();
  if (idx >= cnt) return false;
  Preferences prefs;
  if (!prefs.begin(_ns.c_str(), true)) return false;
  int head = prefs.getInt("hist_head", 0);
  int oldest = (cnt < _cap) ? 0 : head;
  int slot = (oldest + idx) % _cap;
  char key[32];
  snprintf(key, sizeof(key), "hist_%02d", slot);
  out = prefs.getString(key, "");
  prefs.end();
  return out.length() > 0;
}

int HistoryStore::readRange(int startIdx, int n, const std::function<bool(int, const String&)>& cb) {
  if (startIdx < 0 || n <= 0) return 0;
  Preferences prefs;
  if (!prefs.begin(_ns.c_str(), true)) return 0;
  int head = prefs.getInt("hist_head", 0);
  int cnt  = prefs.getInt("hist_cnt", 0);
  int oldest = (cnt < _cap) ? 0 : head;
  int done = 0;
  for (int i = 0; i < n && startIdx + i < cnt; i++) {
    int slot = (oldest + startIdx + i) % _cap;
    char key[32];
    snprintf(key, sizeof(key), "hist_%02d", slot);
    String line = prefs.getString(key, "");
    if (!cb(startIdx + i, line)) { done++; break; }
    done++;
  }
  prefs.end();
  return done;
}

void HistoryStore::clear() {
  Preferences prefs;
  if (!prefs.begin(_ns.c_str(), false)) return;
  for (int i = 0; i < _cap; i++) {
    char key[32];
    snprintf(key, sizeof(key), "hist_%02d", i);
    if (prefs.isKey(key)) prefs.remove(key);
  }
  prefs.putInt("hist_head", 0);
  prefs.putInt("hist_cnt", 0);
  prefs.end();
}
