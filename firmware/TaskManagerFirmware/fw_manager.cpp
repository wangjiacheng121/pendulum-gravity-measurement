// =====================================================================
// fw_manager.cpp —— 程序管理器实现
// =====================================================================
#include "fw_manager.h"
#include "fw_config.h"
#include "fw_store.h"
#include "fw_display.h"
#include <Wire.h>

AppManager Manager;

const char* Mgr_activeName() { return Manager.activeName(); }

void AppManager::registerApp(App* app) {
  _apps.push_back(app);
  EventBus::subscribe(app);
}

App* AppManager::findById(AppId id) {
  for (auto* a : _apps) if (a->id() == id) return a;
  return nullptr;
}

const char* AppManager::activeName() const {
  return _active ? _active->name() : "-";
}

bool AppManager::screenOff() const {
  return Display.asleep();
}

void AppManager::begin() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  _btnRawLast = _btnStable = (digitalRead(PIN_BUTTON) == HIGH);
  _btnChangeMs = millis();

  _saverTimeoutS = Store.getInt("sys.screen_saver_timeout", SCREEN_SAVER_DEFAULT_S);
  _lastInteract = millis();
  _lastTickMs = millis();
  _lastBtnMs = millis();

  // 硬约束：上电无条件默认激活程序1（桌宠），不记忆上次程序
  activate(AppId::PET, "boot");

  // 预创建其余程序（仅 onCreate：恢复状态 + 注册 BLE 菜单，不给屏幕不 onTick）
  // 保证开机后 BLE [2][3][4] 菜单即可用；onShow 仍延迟到首次切入
  for (auto* a : _apps) {
    AppId id = a->id();
    if (!_created[(int)id]) {
      a->onCreate();
      _created[(int)id] = true;
      Serial.printf("[Mgr] pre-created %s\n", a->name());
    }
  }
}

void AppManager::activate(AppId id, const char* reason) {
  App* target = findById(id);
  if (!target) {
    Serial.printf("[Mgr] ERR: app %d not registered\n", (int)id);
    return;
  }
  if (_active == target) return;

  uint32_t t0 = millis();
  if (_active) {
    _active->onHide();               // 契约：同步完成——写NVS/注销中断/归还屏幕
  }
  Display.grant(id);                 // 屏幕独占授权转移
  if (!_created[(int)id]) {
    target->onCreate();              // 首次激活前：NVS 恢复状态
    _created[(int)id] = true;
  }
  _active = target;
  _active->onShow();                 // 重绘首屏、挂载中断/定时器
  _lastInteract = millis();
  Serial.printf("[Mgr] activate %s (%.1fms) reason=%s\n",
                target->name(), (float)(millis() - t0), reason);
}

void AppManager::tick() {
  uint32_t now = millis();

  // ---- 按键状态机：100Hz 独立节拍（2026-10-02 长按失效修复）----
  // 原实现里 handleButton() 混在 100ms 的 onTick 里跑，导致：
  //   ① 短按（150-250ms）横跨多个采样窗，采样点稀疏；
  //   ② 长按 1.2s 只有约 10 个采样点，叠加天气程序 15-25s 阻塞后基本等不到。
  // 现在按键按 LOOP_INTERVAL_MS(10ms) 采样，与 onTick 业务节拍解耦。
  if ((int32_t)(now - _lastBtnMs) >= LOOP_INTERVAL_MS) {
    _lastBtnMs = now;
    handleButton();
  }

  // ---- onTick 业务分发：仍节流在 TICK_INTERVAL_MS(100ms)，契约不变 ----
  if ((int32_t)(now - _lastTickMs) < TICK_INTERVAL_MS) return;
  float dt = (now - _lastTickMs) / 1000.0f;
  _lastTickMs = now;

  // ⚠️ 必须在 handleButton() 之后重新取时间（2026-10-02 实测修复）
  // 屏保判据 (now - _lastInteract) 要求 now 不早于 _lastInteract。但长按会在
  // handleButton() 里调 activate()，而 activate() 末尾把 _lastInteract = millis()
  // 推到「比函数开头那个 now 更晚」的时刻（activate 要写 NVS + 整帧提交，耗时 1-33ms）。
  // 若沿用开头的旧 now，无符号减法下溢成接近 4.29e9 的巨值 > timeout，
  // 屏保立刻误判超时，把刚切过去的程序又弹回桌宠 —— 上板日志实测复现：
  //   LONG fired → activate 单摆测g(4ms) → L0 screen-saver → activate 桌宠
  now = millis();

  // L0 屏保回家：程序2/3 无交互超时自动切回程序1
  //  - 程序1 home 态自身不触发
  //  - 测量进行中（程序2 busy）不触发
  //  - timeout=0 关闭
  if (_active && _active->id() != AppId::PET &&
      _saverTimeoutS > 0 && !_active->isBusy() &&
      Display.present() && !Display.asleep() &&
      (int32_t)(now - _lastInteract) > (int32_t)_saverTimeoutS * 1000) {
    Serial.println("[Mgr] L0 screen-saver -> app1");
    activate(AppId::PET, "screensaver");
  }

  // onTick 分发（≤10Hz）
  if (_active) _active->onTick(dt);
}

// ---------------- 按键状态机 ----------------
void AppManager::handleButton() {
  bool raw = (digitalRead(PIN_BUTTON) == HIGH);   // true=未按下
  uint32_t now = millis();

  // 消抖：电平稳定 ≥ BTN_DEBOUNCE_MS 才接受新状态
  if (raw != _btnRawLast) {
    _btnRawLast = raw;
    _btnChangeMs = now;
  }
  if (raw != _btnStable && (now - _btnChangeMs) >= BTN_DEBOUNCE_MS) {
    _btnStable = raw;
    if (!_btnStable) {
      // ---- 按下沿 ----
      _pressed = true;
      _pressStartMs = now;
      _longFired = false;
      _superFired = false;
      _lastInteract = now;
      Serial.println("[Btn] press down");
    } else {
      // ---- 释放沿 ----
      _pressed = false;
      uint32_t heldMs = now - _pressStartMs;
      if (!_longFired && !_superFired && _active) {
        // 短按：
        if (Display.asleep()) {
          // L3 唤醒：任意按键只唤醒，不转发主功能（分册01第4节）
          Display.wake();
          _active->onWake();
          _lastInteract = millis();
          Serial.println("[Mgr] wake from L3 (press consumed)");
        } else {
          Serial.printf("[Btn] short %ums -> %s\n", (unsigned)heldMs, _active->name());
          _active->onButton();
          _lastInteract = millis();
        }
      } else {
        Serial.printf("[Btn] release %ums (long=%d)\n", (unsigned)heldMs, _longFired ? 1 : 0);
      }
    }
  }

  // 按住期间的超时判定
  if (_pressed) {
    uint32_t held = now - _pressStartMs;
    if (!_superFired && held >= BTN_SUPER_MS) {
      // 超长按 ≥5s：系统重启（总纲硬约束，预留）
      _superFired = true;
      Serial.println("[Mgr] super-long-press 5s -> RESTART");
      Display.clearBuffer();
      Display.setFont(DisplayService::FONT_MID);
      Display.centerStr(38, "Restarting...");
      Display.commit();
      delay(300);
      ESP.restart();
    }
    if (!_longFired && held >= BTN_LONG_MS) {
      // 长按 ≥0.8s：切换程序（管理器拦截，不转发）
      _longFired = true;
      // 不要在这里写 _lastInteract = now：那里的 now 是本函数入口时刻，
      // 一定早于 activate() 内部 millis()（activate 要写 NVS + 整帧提交，耗时可达 33ms），
      // 写入反而会把 _lastInteract 固定在一个偏旧的值上。activate() 末尾已正确更新。
      if (Display.asleep()) {
        Display.wake();
        if (_active) _active->onWake();
      }
      AppId cur = _active ? _active->id() : AppId::PET;
      AppId next;
      switch (cur) {
        case AppId::PET:      next = AppId::PENDULUM; break;
        case AppId::PENDULUM: next = AppId::WEATHER;  break;
        default:              next = AppId::PET;      break;
      }
      Serial.printf("[Btn] LONG %ums fired\n", (unsigned)held);
      activate(next, "long-press");
    }
  }
}

// ---------------- EventBus 实现 ----------------
static std::vector<App*>* subs() {
  static std::vector<App*> s_subs;
  return &s_subs;
}

void EventBus::subscribe(App* app) {
  subs()->push_back(app);
}

void EventBus::broadcast(const Event& evt) {
  // 广播到全部已注册程序（非活跃程序零调度，但事件驱动回调允许，
  // 这是程序1 表情联动的通道——总纲 5.3）
  for (auto* a : *subs()) a->onEvent(evt);
}
