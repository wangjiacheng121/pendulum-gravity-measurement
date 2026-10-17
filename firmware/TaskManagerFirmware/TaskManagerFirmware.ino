// =====================================================================
// TaskManagerFirmware.ino —— 主入口
// 任务管理器式多程序固件：程序1 桌宠（默认） / 程序2 单摆测g / 程序3 天气
// 依据《00-总纲-架构与硬件协议》《01-桌宠》《02-单摆测g》《03-天气》实现
//
// 上电时序（总纲 5.5）：
//   初始化外设与NVS → 读全局配置 → 注册三程序 → 激活程序1（桌宠）
//   → onCreate 恢复状态 → 屏保待机显示
//   SNTP 对时与 WiFi 重连后台异步，不阻塞首屏
// =====================================================================
#include <Wire.h>
#include "fw_config.h"
#include "fw_types.h"
#include "fw_store.h"
#include "fw_time.h"
#include "fw_display.h"
#include "fw_net.h"
#include "fw_ble.h"
#include "fw_manager.h"
#include "app1_pet.h"
#include "app2_pendulum.h"
#include "app3_weather.h"

static PetApp      app1Pet;       // 程序1 桌宠屏保（默认·home·屏保）
static PendulumApp app2Pendulum;  // 程序2 单摆测g（GPIO4 中断活跃时挂载）
static WeatherApp  app3Weather;   // 程序3 实时天气（30min 周期 + 缓存降级）

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== " FW_NAME " v" FW_VERSION " ===");

  // I2C：ESP32-S3 必须显式指定 8/9（默认 21/22 不匹配，总纲第 3 节）
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  Store.begin();      // NVS：出厂默认值（天气凭据内置，WiFi 用内置回退列表）
  Display.begin();    // SH1106 专用驱动（探测 0x3C/0x3D）
  Time.begin();       // SNTP 异步对时
  Net.begin();        // 网络服务（不主动连接；凭据=NVS 用户配置 + 内置默认回退列表）
  Ble.begin();        // BLE NUS 常备（pendulum-g）

  // 注册三程序（同时订阅事件总线；各程序在 onCreate 中注册自己的 BLE 菜单：
  // 程序1→[5]AI桩  程序2→[2]摆长 [3]历史  程序3→[4]天气）
  Manager.registerApp(&app1Pet);
  Manager.registerApp(&app2Pendulum);
  Manager.registerApp(&app3Weather);

  Manager.begin();    // 无条件激活程序1（桌宠），不记忆上次程序
}

// 串口命令行累积（2026-10-02）
// 背景：本机 Windows 蓝牙栈对该设备 GATT 不兼容（手机 nRF Connect 可稳定连），
// 串口是唯一可靠的验证通道，故让串口复用 BLE 的全部命令语义。
// 注意：绝不能用 readStringUntil() 等阻塞读——主循环有 100ms 节拍且 onTick 在其中跑，
// 阻塞会连带冻结光电门中断的消费与屏幕刷新。逐字节非阻塞 + 手动切行。
static String s_cliLine;
static uint32_t s_cliLastMs = 0;

static void cliSerialPump() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;                 // 兼容 CRLF
    if (c == '\n') {
      String cmd = s_cliLine;
      s_cliLine = "";
      cmd.trim();
      if (cmd.length()) {
        Serial.println("[CLI<] " + cmd);
        Ble.pushSerialLine(cmd);             // 入同一队列 → tick() 走既有路由
      }
      continue;
    }
    // 可打印字符才收，防止二进制噪声灌满 String
    if (c >= 32 && c < 127) {
      if (s_cliLine.length() < 120) s_cliLine += (char)c;   // 命令行长度上限，防止无换行的长跑导致内存增长
    } else {
      s_cliLine = "";                        // 收到不可打印字符 → 丢弃该行（抗乱码）
    }
  }
  // 看门狗：4s 内没有换行且行未清空 → 视为残缺命令，丢弃，防止半条命令一直挂着
  if (s_cliLine.length() && millis() - s_cliLastMs > 4000) {
    Serial.println("[CLI] 残缺行已丢弃: " + s_cliLine);
    s_cliLine = "";
  }
  if (Serial.available() == 0) s_cliLastMs = millis();
}

void loop() {
  // 节拍 10ms（2026-10-02）：loop 自身跑快，只为把按键采样率提到 100Hz。
  // Manager::tick() 内部已把「按键采样(10ms)」与「onTick 业务分发(100ms)」分离，
  // 业务重活（整帧提交/NVS/BLE 分块）不会被这个高频节拍带起来。
  // 天气程序的 HTTP 已迁到独立 FreeRTOS 任务（fw_net.cpp），主循环不再被 15-25s 阻塞。
  cliSerialPump();  // 串口命令入队（非阻塞）
  Manager.tick();     // 按键 100Hz + onTick ≤10Hz + 屏保检查
  Net.tick();         // 连接结果 / 空闲 5min 断开 / 扫描
  Ble.tick();         // BLE 行路由 + 连接事件 + 配网会话
  delay(LOOP_INTERVAL_MS);
}
