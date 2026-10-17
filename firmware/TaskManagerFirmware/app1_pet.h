#pragma once
// =====================================================================
// app1_pet.h —— 程序1「桌宠屏保 / home」（分册 01，必做部分）
// 职责：
//  - 开机默认激活程序 + 空闲屏保 + home
//  - 颜文字表情状态机（每态≥2帧，1-2fps，500-1000ms 交替）
//  - 按键互动（短按 happy / 3s 内连按 3 次 spin 彩蛋）
//  - 全局事件联动：测量完成→cheer、天气更新→wxok/sad、断网→sad
//  - 非活跃时：表情以通知条形式叠加在当前程序画面角落（不抢屏）
//  - 瞌睡熄屏协作：idle 5min→sleepy，再 1min→请求熄屏（L3）
// 可选模块：
//  - RSS 新闻（未实现，接口预留）
//  - AI 问答：仅 BLE [5] 菜单桩（分册01第6节，禁止引入小智固件）
// NVS 键：app1.pet_mood（表情状态断电恢复）
// BLE 菜单：[5] AI问答（未启用）
// =====================================================================
#include "fw_types.h"

class PetApp : public App {
public:
  AppId     id()   const override { return AppId::PET; }
  const char* name() const override { return "桌宠"; }

  // 表情状态（public：NVS 恢复辅助函数 moodFromStr 需要在类外使用）
  enum class PetState : uint8_t {
    IDLE, HI, HAPPY, SPIN, SLEEPY, CHEER, WXOK, SAD
  };

  void onCreate() override;          // 恢复 app1.pet_mood
  void onShow()   override;          // 全屏演表情：上电/切入 → hi 3s
  void onHide()   override;          // 状态写 NVS
  void onButton() override;          // 短按互动
  void onTick(float dt) override;    // 帧动画 + 转移计时
  void onEvent(const Event& evt) override;
  void onWake()   override;          // 熄屏唤醒当次（不触发主功能）
  bool onBleMsg(const String& line, String& reply) override;
  bool isBusy() const override { return false; }

private:
  void setState(PetState s, uint32_t durMs = 0);
  void draw();

  PetState _state = PetState::IDLE;
  bool     _frame = false;                // 双帧交替
  uint32_t _stateSinceMs = 0;             // 当前态进入时刻
  uint32_t _stateDurMs = 0;               // 0=常驻（idle/sad/sleepy），否则到时回 idle
  uint32_t _lastFrameMs = 0;

  uint32_t _pressTimes[3] = {0,0,0};      // 连按 3 次彩蛋判定
  bool     _wokeFromSleep = false;        // 本次唤醒后不立即重新入睡计时基准问题

  // 熄屏协作
  bool     _offRequested = false;         // 已请求熄屏
};
