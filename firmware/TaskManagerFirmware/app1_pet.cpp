// =====================================================================
// app1_pet.cpp —— 程序1「桌宠屏保」实现（分册 01）
// 颜文字采用 ASCII 字形（u8g2 10x20 字体无全角字形；
// 分册允许"微调字形，保持颜文字风格"）
// =====================================================================
#include "app1_pet.h"
#include "fw_config.h"
#include "fw_store.h"
#include "fw_display.h"
#include "fw_time.h"
#include "fw_manager.h"
#include "fw_ble.h"

// 真实运行的实例在 TaskManagerFirmware.ino 中定义（静态对象）；
// 此处不再另建全局副本——否则每个程序都会多占一份 RAM 且极易被误当成运行实例。

// ---------- 帧表（每态 ≥2 帧，SLEEPY 静帧第二帧同款） ----------
static const char* const FRAMES[8][2] = {
  /* IDLE   */ { "^(-.-)^",   "^(o_o)^"   },   // 慢眨眼
  /* HI     */ { "(^o^)/",    "(^o^)/~"   },
  /* HAPPY  */ { "(*^_^*)",   "\\(^_^)/"  },
  /* SPIN   */ { ">(^o^)/~",  "~\\(^o^)<" },   // 左右横跳模拟转圈
  /* SLEEPY */ { "(-_-) zzZ", "(-_-) zzZ" },
  /* CHEER  */ { "\\(^o^)/",  "\\o(^o^)o/" },
  /* WXOK   */ { "(^-^*)",    "(*^v^*)"   },
  /* SAD    */ { "(T_T)",     "(T_T) ;"   },
};

static const char* STATE_NAME[8] = {
  "idle", "hi", "happy", "spin", "sleepy", "cheer", "wxok", "sad"
};

static PetApp::PetState moodFromStr(const String& s) {
  if (s == "sad")    return PetApp::PetState::SAD;
  if (s == "idle")   return PetApp::PetState::IDLE;
  return PetApp::PetState::IDLE;   // 其他瞬时态重启后无意义，一律回 idle
}

// ===================== 生命周期 =====================

void PetApp::onCreate() {
  // 注册本程序 BLE 菜单项：[5] AI 问答桩（分册01第6节）
  Ble.registerMenu("5", "AI 问答（未启用）", this);
  // 断电重启：从上次的 idle/sad 等合理态恢复（分册01第3.2节）
  String mood;
  Store.getString("app1.pet_mood", mood);
  PetState s = moodFromStr(mood);
  _state = s; _stateDurMs = 0;
  _offRequested = false;
  Serial.printf("[Pet] onCreate, mood=%s\n", mood.c_str());
}

void PetApp::onShow() {
  // 上电首次 onShow / 从程序2、3 切入 → 打招呼 hi 3s（分册01第3.1节）
  setState(PetState::HI, 3000);
  draw();
}

void PetApp::onHide() {
  // 状态已随每次 setState 写 NVS；此处再兜底写一次
  Store.setString("app1.pet_mood", STATE_NAME[(int)_state]);
}

void PetApp::onWake() {
  // 熄屏唤醒当次（管理器不转发主功能）：回 idle，取消熄屏
  _offRequested = false;
  setState(PetState::IDLE, 0);
  draw();
}

// ===================== 状态机 =====================

void PetApp::setState(PetState s, uint32_t durMs) {
  _state = s;
  _frame = false;
  _stateSinceMs = millis();
  _stateDurMs = durMs;
  if (s != PetState::IDLE) _offRequested = false;
  // 仅持久化常驻态（idle/sad/sleepy）；happy/spin 等 3s 瞬时态不写 NVS——
  // 重启后 moodFromStr 本就只认 idle/sad，逐次写 Flash 纯磨损（优化）
  if (s == PetState::IDLE || s == PetState::SAD || s == PetState::SLEEPY) {
    Store.setString("app1.pet_mood", STATE_NAME[(int)s]);
  }
}

void PetApp::onButton() {
  uint32_t now = millis();

  if (_state == PetState::SLEEPY) {
    // 瞌睡态任意按键 → idle，并取消熄屏请求（分册01第3.2节）
    _offRequested = false;
    setState(PetState::IDLE, 0);
    draw();
    return;
  }
  if (_state != PetState::IDLE) return;   // 短暂态期间不响应互动

  // 连按 3 次（3s 内）→ 彩蛋 spin 2s
  _pressTimes[2] = _pressTimes[1];
  _pressTimes[1] = _pressTimes[0];
  _pressTimes[0] = now;
  if (_pressTimes[2] != 0 && now - _pressTimes[2] <= 3000) {
    _pressTimes[0] = _pressTimes[1] = _pressTimes[2] = 0;
    setState(PetState::SPIN, 2000);
    draw();
    return;
  }

  // 普通：短按 → happy 3s
  setState(PetState::HAPPY, 3000);
  draw();
}

void PetApp::onTick(float dt) {
  (void)dt;
  uint32_t now = millis();
  if (!Display.present()) return;
  if (Display.asleep()) return;           // 熄屏期间不耗帧

  // 瞌睡链路（仅 pet 活跃时计时基准 = 管理器 lastInteraction）
  if (_state == PetState::IDLE) {
    if (now - Manager.lastInteractionMs() >= PET_IDLE_TO_SLEEPY_MS) {
      setState(PetState::SLEEPY, 0);      // 5min 无操作 → 瞌睡
    }
  } else if (_state == PetState::SLEEPY) {
    if (!_offRequested &&
        now - _stateSinceMs >= PET_SLEEPY_TO_OFF_MS) {
      _offRequested = true;
      Display.sleep();                    // 瞌睡 1min → 请求显示服务熄屏（L3）
      return;
    }
  }

  // 短暂态计时到 → 回 idle（SAD/SLEEPY 常驻，恢复事件驱动退出）
  if (_stateDurMs > 0 && now - _stateSinceMs >= _stateDurMs) {
    setState(PetState::IDLE, 0);
  }

  // 帧动画：500-1000ms 交替（≤2fps）
  if (now - _lastFrameMs >= 700) {
    _lastFrameMs = now;
    _frame = !_frame;
    draw();
  }
}

void PetApp::onEvent(const Event& evt) {
  bool iAmActive = (Manager.activeId() == AppId::PET);

  switch (evt.type) {
    case EventType::EVT_MEASURE_DONE:
      // 庆祝 + 通知条含本次 g（分册01第3.1/3.2节）
      Display.showNotification("\\(o^) G=" + String(evt.value, 2), 5000);
      if (iAmActive) setState(PetState::CHEER, 5000);
      break;

    case EventType::EVT_WX_UPDATE_OK:
      Display.showNotification("(^v^) 天气已更新", 3000);
      if (iAmActive) setState(PetState::WXOK, 3000);
      break;

    case EventType::EVT_WX_UPDATE_FAIL:
      Display.showNotification("(T_T) 天气更新失败 E" + String(evt.code), 4000);
      if (iAmActive) setState(PetState::SAD, 0);
      break;

    case EventType::EVT_NET_LOST:
      // L2 空闲 5min 主动断电是省电动作，不是故障：不进 Sad，否则桌宠会长期挂着"联网异常"
      if (evt.code == EVT_NET_LOST_CODE_POWERSAVE) {
        if (iAmActive) setState(PetState::IDLE, 0);
        break;
      }
      Display.showNotification("(T_T) 网络断开", 4000);
      if (iAmActive) setState(PetState::SAD, 0);   // 持续至恢复
      break;

    case EventType::EVT_NET_GOT:
    case EventType::EVT_BLE_CONN:
      // sad 长驻恢复 → 回 idle
      if (_state == PetState::SAD) setState(PetState::IDLE, 0);
      break;

    case EventType::EVT_BLE_DISC:
    default:
      break;
  }
}

// ===================== BLE 菜单 =====================

bool PetApp::onBleMsg(const String& line, String& reply) {
  if (line == "5") {
    // AI 问答桩（分册01第6节）：仅预留，未启用，禁止引入小智固件依赖
    reply = "INFO: AI 模块未启用";
    return true;
  }
  return false;   // 非本程序命令
}

// ===================== 绘制 =====================

void PetApp::draw() {
  if (!Display.present()) return;
  Display.clearBuffer();
  Display.setFont(DisplayService::FONT_BIG);
  String face = FRAMES[(int)_state][_frame ? 1 : 0];
  Display.centerStr(34, face);

  // 底部小字：时间 / 状态标注（信息只许角落小字，不破坏主画面）
  Display.setFont(DisplayService::FONT_SMALL);
  String tag = Time.hhmm();
  if (_state == PetState::SAD)   tag += "  联网异常";
  if (_state == PetState::SLEEPY) tag += "  zzz";
  Display.drawStr(2, 63, tag);

  Display.commit();   // 通知条由显示服务自动叠加
}
