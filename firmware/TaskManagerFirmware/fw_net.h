#pragma once
// =====================================================================
// fw_net.h —— 网络服务（总纲第 6 节）
// 契约：
//  - 程序只发「连接请求」，不碰 WiFi 驱动
//  - 连接/断开广播 EVT_NET_GOT / EVT_NET_LOST
//  - WiFi 空闲 5 分钟自动断开并进调制解调器休眠（L2，总纲 5.4）
//  - HTTP GET 异步化：阻塞部分跑在独立 FreeRTOS 任务，主循环只发起/取结果
//    （2026-10-02：原同步实现阻塞主循环 15-25s，导致按键完全失灵）
//  - 凭据策略（2026-09-30 修订）：NVS 用户凭据（BLE 配网）优先，其余为 fw_config.h
//    内置默认回退列表；连接失败自动轮换下一个。
//    WiFi 默认列表属个人私有信息，公开源码前必须清空 fw_config.h 中 WIFI_DEF_*
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "fw_config.h"              // WIFI_CRED_MAX / WIFI_DEF_*（默认回退凭据列表）

class NetService {
public:
  void begin();                       // 读 NVS 凭据、注册 WiFi 事件；不主动连接
  void tick();                        // 状态机：连接结果 / 空闲断开 / 扫描完成

  // ---- 连接管理（程序侧唯一入口）----
  void requestConnect();              // 按凭据列表连接（用户凭据优先，失败自动轮换；幂等）
  void connectWith(const String& ssid, const String& pass, bool saveToNvs = true);
  void keepAlive();                   // 活跃标记（推迟 5 分钟空闲断开）
  bool connected() const { return _state == ST_CONNECTED; }
  bool connecting() const { return _state == ST_CONNECTING; }
  bool credsSaved() const { return _ssid.length() > 0; }   // 是否有 NVS 用户凭据
  bool hasAnyCred() const { return _listN > 0; }           // 用户凭据或内置默认任一可用
  String statusText();
  String ssid() const { return _ssid; }                    // NVS 用户凭据 SSID（可空）
  String currentTarget() const;                            // 当前正在/将要尝试的 SSID
  void radioOffIfIdle();               // 空闲态则关射频（配网/扫描收尾调用，遵守 L2 省电）

  // ---- 异步扫描（BLE 配网用）----
  void startScan();
  bool scanDone();
  int  scanCount() { return WiFi.scanComplete(); }
  String scanSsid(int i) { return WiFi.SSID(i); }
  int  scanRssi(int i)   { return WiFi.RSSI(i); }

  // ---- 异步 HTTP（2026-10-02 新增，取代阻塞式 httpGet）----
  // 背景：原 httpGet() 同步阻塞 15-25s（等连 + GET + 读流 + 解压），
  // 而它在程序3 的 onTick 里被调用 → 主循环整段停摆 → 按键状态机停止采样，
  //   表现为「长按切程序无效」+「天气短按不换屏」。
  // 改为：请求投递给独立 FreeRTOS 任务执行，主循环只负责发起与取结果，永不阻塞。
  // 用法：① httpStart() 发起（忙则返回 false）② 之后每 tick 调 httpPoll() 取结果
  // 约束：httpPoll() 取回的 String 由主循环线程使用；同一时刻只允许一个在飞请求。
  bool httpStart(const String& url, const String& hostForDnsCheck, bool gunzip = false);
  bool httpPoll(int& outCode, String& outBody);   // true=有结果（已取出并清标志）
  bool httpBusy() const { return _httpReqPending; }

  // ---- HTTP GET 内部实现（仅供网络任务调用，不对外） ----
  int  httpGetBlocking(const String& url, const String& hostForDnsCheck,
                       String& outBody, bool gunzip);

private:
  enum State : uint8_t { ST_IDLE, ST_CONNECTING, ST_CONNECTED };
  void loadCreds();
  void rebuildList();                 // 组装凭据列表（用户 NVS 优先 + 内置默认，去重）
  void rotateOrStop();                // 当前凭据失败 → 轮换下一个；整轮失败回 IDLE
  static bool gunzipBytes(const uint8_t* src, size_t srclen, String& out);
  static void httpTaskEntry(void* arg);   // 网络任务主体（阻塞操作全在此）

  // 异步 HTTP 跨线程共享状态（网络任务写 / 主循环读，均经 _httpMux 保护）
  volatile bool _httpReqPending = false;  // 有请求在飞
  volatile bool _httpResReady   = false;  // 有结果待取
  // R6-01 请求序号（2026-10-05）：httpStart 时 ++，结果回填时原样带回。
  // httpPoll 只交付 _httpResSeq == _httpSeq 的结果——若曾有一次结果被错位消费
  // （如幻影信号量），后续所有结果都会整体错位一格：now 的 body 被 24h 阶段
  // 吞掉 → compactHourly 找不到 fxTime → 「保留旧缓存」→ 降水屏永不更新。
  // 实测复现于 2026-10-05 串口日志（详见当日排查记录）。
  volatile uint32_t _httpSeq    = 0;      // 当前在飞请求的序号（主循环写）
  volatile uint32_t _httpResSeq = 0;      // 已完成结果携带的序号（网络任务写）
  int     _httpResCode = 0;
  String  _httpResBody;

  State   _state = ST_IDLE;
  String  _ssid, _pass;               // NVS 用户凭据（BLE 配网），可为空
  String  _listSsid[WIFI_CRED_MAX];   // 回退列表：[0]=用户凭据，其余=内置默认
  String  _listPass[WIFI_CRED_MAX];
  int     _listN   = 0;               // 列表条数
  int     _listIdx = 0;               // 当前尝试的列表下标（跨连接周期保持，天然漫游）
  uint8_t _failRun = 0;               // 连续失败计数（整轮失败即停）
  uint32_t _lastActivityMs = 0;       // keepAlive 时间戳
  volatile uint32_t _suppressLostUntilMs = 0;  // 凭据轮换期间抑制 EVT_NET_LOST（WiFi 任务读）
  volatile bool _evtGotPending  = false;  // WiFi 事件延后标志（loop 上下文广播，见 fw_net.cpp）
  volatile bool _evtLostPending = false;
};

extern NetService Net;
