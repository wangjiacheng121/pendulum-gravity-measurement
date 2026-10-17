#pragma once
// =====================================================================
// fw_store.h —— 存储服务（NVS / Preferences）
// 契约（总纲第 6、7 节）：
//  - 键值 get/set，命名空间强制 sys. / app1. / app2. / app3.（点号分隔）
//  - 历史记录 append/list/export/clear 统一由存储服务管理（环形 ≤50 组）
// =====================================================================
#include <Arduino.h>
#include <functional>

class StoreService {
public:
  void begin();   // 初始化并写入出厂默认值（仅当键不存在时）

  // dottedKey 形如 "sys.wifi_ssid" / "app1.pet_mood"
  bool getString (const String& dottedKey, String& out);
  void setString (const String& dottedKey, const String& val);
  bool hasKey    (const String& dottedKey);
  void removeKey (const String& dottedKey);

  int32_t  getInt   (const String& dottedKey, int32_t  def);
  void     setInt   (const String& dottedKey, int32_t  val);
  double   getDouble(const String& dottedKey, double   def);
  void     setDouble(const String& dottedKey, double   val);

private:
  bool split(const String& dottedKey, const char*& ns, String& key); // 校验前缀合法性
};

// =====================================================================
// HistoryStore —— 通用环形历史记录（程序2 实验记录用，app2 命名空间）
// 物理布局：hist_00 .. hist_(cap-1) + hist_head（写入游标）+ hist_cnt
// =====================================================================
class HistoryStore {
public:
  explicit HistoryStore(const char* ns, int cap = 50);
  void begin();
  void append(const String& line);                 // FIFO 环形写入（一行一条，≤200字符）
  int  count();                                    // 当前有效条数
  // 旧→新遍历；cb 返回 false 提前终止；idx 为逻辑序号（0=最旧）
  void forEach(const std::function<bool(int idx, const String&)>& cb);
  bool getAt(int idx, String& out);                // 按逻辑序号取（0=最旧）；失败返回 false
  // 批量读：单次 open Preferences 读 n 条（idx 无论是否空行都推进，保证调用方能收敛）。
  // 导出场景若用 getAt 逐条取，每条内部还会额外 open 一次取 count()，
  // 10Hz × 8 行 ≈ 160 次/秒 NVS 开关。
  int  readRange(int startIdx, int n, const std::function<bool(int idx, const String& line)>& cb);
  void clear();

private:
  String _ns;
  int    _cap;
};

extern StoreService Store;
