#pragma once
// =====================================================================
// fw_manager.h —— 程序管理器（总纲第 5 节，主）
// 职责：
//  - 注册表与生命周期分发（onCreate/onShow/onHide/onTick/onButton/onEvent）
//  - 按键路由：短按→转发当前程序；长按≥1.2s→切换程序；超长按≥5s→重启
//  - L0 屏保回家：程序2/3 无交互 > sys.screen_saver_timeout 自动切回程序1
//    （程序1 home 态不触发；测量进行中不触发）
//  - L3 熄屏唤醒：唤醒当次只唤醒，不转发主功能
//  - 上电无条件默认激活程序1（不记忆"上次程序"，总纲硬约束）
// =====================================================================
#include <Arduino.h>
#include <vector>
#include "fw_types.h"

class AppManager {
public:
  void begin();                       // 上电时序：激活程序1（桌宠）
  void registerApp(App* app);         // 注册 + EventBus 订阅
  void activate(AppId id, const char* reason = "");
  void tick();                        // 按键状态机 + onTick 分发 + 屏保检查

  App*    active() const { return _active; }
  AppId   activeId() const { return _active ? _active->id() : AppId::PET; }
  const char* activeName() const;
  uint32_t lastInteractionMs() const { return _lastInteract; }
  bool screenOff() const;             // L3 熄屏中

private:
  void handleButton();
  App* findById(AppId id);

  std::vector<App*> _apps;
  App*     _active = nullptr;
  bool     _created[4] = { false, false, false, false };  // index=AppId

  // 按键状态机
  bool     _btnStable = true;         // true=未按下（上拉）
  bool     _btnRawLast = true;
  uint32_t _btnChangeMs = 0;
  bool     _pressed = false;
  uint32_t _pressStartMs = 0;
  bool     _longFired = false;
  bool     _superFired = false;

  uint32_t _lastInteract = 0;
  uint32_t _lastTickMs = 0;
  uint32_t _lastBtnMs = 0;           // 按键专用节拍戳（与 onTick 节拍解耦，2026-10-02）
  int32_t  _saverTimeoutS = 180;
};

extern AppManager Manager;
extern const char* Mgr_activeName();    // BLE 状态页用的桥接函数
