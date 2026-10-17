# 单摆测重力加速度 · ESP32-S3 免焊接实验装置

一个面向高中物理（人教版「单摆测量重力加速度 g」）的**免焊接、低门槛**开源实验装置：
用 ESP32-S3 + 面包板 + 光电门，配套三合一固件（桌宠屏保 / 单摆测 g / 实时天气），
把「单摆法测 g」做成可重复、可读数、可上云的实验平台。

> 本项目**衍生自**立创开源（OSHWHub）项目
> [mcitem/pendulum-gravity-measurement](https://oshwhub.com/mcitem/pendulum-gravity-measurement)
> （原项目为 Rust + Embassy 固件，GPL-3.0，代码已开源）。
> 本仓库是其 **C++/Arduino 重写版**，沿用其中点法测量思路并做了多项工程化改造。
> 依 GPL-3.0 协议开源，并保留对上游的署名。

---

## ✨ 功能特性

固件「三程序」长按切换（≥1.2s），非活跃程序自动挂起：

| 程序 | 功能 |
|---|---|
| ① 桌宠屏保 | OLED 像素宠物，作为默认开机界面与闲置屏保 |
| ② 单摆测 g | 光电门边沿采集 → 中点法重建 → 多点平均求 g；支持直接法 / 作图法 |
| ③ 实时天气 | 和风天气 QWeather v7（now + 24h），四屏循环；断网降级走缓存 |

- **采集/重建分层**：ISR 仅记录每一条 GPIO 边沿（零过滤、零丢弃），全部纠错在 `onTick` 完成，抗振荡/双遮挡/粘连。
- **免焊接**：全部元件可插面包板，预算 ≤ ¥150，契合高中 DIY。
- **BLE 调试**：NUS 协议，手机/电脑浏览器即可下发指令、看实时数据（无需 App）。
- **多凭据回退**：WiFi 支持 NVS 配网 + `fw_config.h` 内置默认（开源版已清空，见下文）。

---

## 🧰 硬件清单（免焊接）

- 主控：ESP32-S3 开发板（8MB Flash，如 LCKFB-ESP32S3R8N8），跨插单块 830 面包板
- 显示：0.96" OLED（I²C，SH1106，地址 `0x3C`）
- 传感：对射式光电门（5V 供电，**禁接 3.3V**）+ 8 路 BSS138 电平转换（OUT → GPIO4）
- 电源：MB-102 面包板电源模块（5V 轨） + 3.3V 轨（供 ESP32/OLED/电平转换 VCCA）
- 交互：轻触按键（GPIO5，INPUT_PULLUP）
- 机械：挡光片（≤12mm 软纸或 15~20mm 硬纸 + 5g 砝码），摆球悬于槽外

> 详细逐孔接线、双电源轨与常见坑（光电门极性、I²C 引脚、杜邦虚接等）见
> [`hardware/单摆测g面包板逐孔接线详解.html`](hardware/单摆测g面包板逐孔接线详解.html)。
> 3D 打印外壳图纸与 STL 见 [`hardware/`](hardware/)（`3D打印外壳图纸…html` 与 `enclosure/`）。

---

## 🗂 目录结构

```
pendulum-gravity-opensource/
├── firmware/TaskManagerFirmware/   # 固件源码（已脱敏）
│   ├── *.ino *.h *.cpp             # 入口 + 服务层(fw_*) + 三程序(app1/2/3_*)
│   ├── tools/                      # 自检/仿真脚本（selftest_rebuild.py 等）
│   └── docs/                       # 各程序说明
├── hardware/                       # 接线详解 + 3D 外壳 + STL
├── docs/
│   ├── design-spec/                # 程序设计规范（一总三分，已脱敏）
│   └── deploy-guide.md             # 换机/部署步骤
├── tools/
│   ├── pc-webui/                   # 电脑端 BLE 网页调试工具
│   └── mobile-webui/               # 手机端 BLE 网页调试工具
├── assets/photos/                  # 实物图
├── LICENSE                         # GPL-3.0
└── README.md
```

---

## 🚀 快速开始：编译与烧录

### 依赖

- [arduino-cli](https://arduino.github.io/arduino-cli/)（需 `--config-file` 指向本机 yaml）
- 板级支持包：`esp32:esp32@3.3.12`（建议 CN 镜像 `3.3.12-cn`）
- 库：`U8g2@2.36.19`（OLED 必须 SH1106 专用驱动）

### 编译

```bash
cd firmware/TaskManagerFirmware
arduino-cli compile --clean --warnings all \
  --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB" \
  --config-file <你的 arduino-cli.yaml> \
  TaskManagerFirmware.ino
```

> ⚠️ **Flash 必须选 8MB**（`default_8MB`）。选 16M / `huge_app` 会导致启动崩溃。
> 编译纪律：必须 `--clean --warnings all` 且 **0 错 0 告警**（增量编译可能给假绿灯）。

### 烧录

```bash
# 自动探测 COM 口，或显式指定
./flash.sh            # 或: arduino-cli upload -p COMx --fqbn <上面的 FQBN> --input-dir <build 目录>
```

更完整的换机/部署流程见 [`docs/deploy-guide.md`](docs/deploy-guide.md)。

---

## 🔑 配置你的凭据（重要）

**开源版已把所有个人凭据清空为占位**，固件开箱即用但**天气功能需要你自己的和风天气 Key**，
WiFi 也建议你用自己的网络。两种配置方式：

### 方式 A：直接改 `fw_config.h`（出厂内置）

```c
#define WIFI_DEF_SSID_1  "你的WiFi名"
#define WIFI_DEF_PASS_1  "你的WiFi密码"
// ... 最多 3 条默认回退
#define WX_HOST_DEFAULT  "你的账号专属域名.re.qweatherapi.com"
#define WX_KEY_DEFAULT   "你的和风天气API Key"
```

### 方式 B：BLE 配网（写入 NVS，优先级最高，无需重刷）

用手机/电脑调试工具连接设备（BLE 名 `pendulum-g`）后，菜单：

- `[1.2]` 配置 WiFi（SSID / 密码）
- `[4]→[4.4]` 配置天气 Host / Key

> 获取和风天气免费凭据（v7）：注册后在控制台「设置 → 凭据管理」拿到
> **凭据 ID / API Key / API Host**（Host 形如 `https://<凭据ID小写>.free.qweatherapi.com`）。
> 详见 [`docs/design-spec/03-程序3-天气.md`](docs/design-spec/03-程序3-天气.md)。

---

## 🛠 调试工具

- **电脑端**：[`tools/pc-webui/`](tools/pc-webui/) — 双击 `启动服务.bat`，浏览器开 `http://127.0.0.1:8777`，
  点「连接 pendulum-g」即可下发 `APP/SIM/GATE/FIX/EDGES` 等指令。
- **手机端**：[`tools/mobile-webui/`](tools/mobile-webui/) — 单文件 `ble_cli_web.html`，
  用 Chrome（Web Bluetooth）直接连。
- **固件自检**：[`firmware/TaskManagerFirmware/tools/selftest_rebuild.py`](firmware/TaskManagerFirmware/tools/selftest_rebuild.py)
  —— **改动光电门重建逻辑前后必跑**，15 个场景全 PASS 才算合格。

指令速查（串口 / BLE 同语义）：`APP <1|2|3>`、`STREAM`、`SIM`、`GATE`、`FIX`、`EDGES`、`HELP`。

---

## 📐 设计文档

- [`docs/design-spec/`](docs/design-spec/) — 程序架构总纲 + 三个程序分册（含单摆中点法修订补丁）。
- [`hardware/`](hardware/) — 面包板逐孔接线、3D 外壳图纸与 STL 模型。

---

## 🔒 隐私与脱敏声明

本仓库**已剔除所有个人私有凭据**，可安全公开：

- `firmware/TaskManagerFirmware/fw_config.h` 中原 3 条 WiFi（SSID+密码）与和风天气 `WX_HOST_DEFAULT` / `WX_KEY_DEFAULT` 共 **5 处**，已清空为占位。
- 设计文档、说明中出现的真实 Key / Host / 凭据 ID 已替换为占位示例。
- **未包含任何预编译 `.bin`**（`firmware/prebuilt/TaskManagerFirmware_R7c.bin` 是带密钥编译的产物，**不对外发布**）。如需成品固件，请按上文自行编译，或由维护者以 GitHub Release 形式单独发布（不含密钥）。

请勿把你的 WiFi 密码、天气 Key 等写回仓库后提交。

---

## 📜 开源协议

- **GPL-3.0** —— 见 [`LICENSE`](LICENSE)。
- 本项目衍生自 [OSHWHub pendulum-gravity-measurement](https://oshwhub.com/mcitem/pendulum-gravity-measurement)（GPL-3.0），依协议保留署名并开源。
- 硬件（面包板接线、3D 外壳 STL）同样以 GPL-3.0 共享。

---

## ⚠️ 免责声明

本装置仅供教学与实验用途。涉及的物理参数（摆长、当地 g）需使用者自行测量校准；
天气功能依赖第三方 API，其可用性不在本仓库保证范围内。
