# 部署指南 · 在另一台电脑重新部署本项目

本指南覆盖：工具链安装 → 固件编译/烧录 → 外壳重印 → BLE 调试工具 → 密钥处理。
原开发机所有工具装在 `E:` 盘（零 C 盘占用），新环境可装到任意盘，只需把下方路径替换为你自己的即可。

---

## A. 工具链总览

| 工具 | 用途 | 版本/备注 |
|---|---|---|
| arduino-cli | 编译/烧录 ESP32 固件 | 任意新版；板卡包 `esp32:esp32@3.3.12`（含 CN 镜像版 `3.3.12-cn`） |
| U8g2 库 | OLED（SH1106）显示 | `2.36.19`，库管理器安装 |
| Python | 运行 tools/ 与 BLE 服务端 | ≥ 3.9（本机用 3.13 隔离 venv） |
| esptool（可选） | 直接烧录预编译 bin | pip 安装 |

---

## B. 安装 arduino-cli 与 ESP32 核心

```bash
# 1) 下载 arduino-cli，放到某目录，例如 D:/tools/arduino-cli/bin/
#    https://github.com/arduino/arduino-cli/releases

# 2) 初始化配置（可选，但建议，便于固定目录与 CN 镜像）
arduino-cli config init --dest-dir D:/tools/arduino-cli/
#   如需 CN 镜像，可在生成的 yaml 里设置 board_manager / libraries 的国内源

# 3) 安装 ESP32 核心（3.3.12；国内可用 3.3.12-cn 镜像版）
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.12

# 4) 安装第三方库
arduino-cli lib install "U8g2@2.36.19"
```

> ⚠️ **Flash 必须选 8MB**（`FlashSize=8M,PartitionScheme=default_8MB`）。
> 选 16M / `huge_app` 分区会导致启动崩溃。板型 = ESP32S3 Dev Module。

---

## C. 编译并烧录固件（推荐：从源码）

项目自带一键脚本 `flash.sh`，把它顶部的路径改成你的环境即可：

```bash
AC="D:/tools/arduino-cli/bin/arduino-cli.exe"
CFG="D:/tools/arduino-cli/arduino-cli.yaml"   # 没有就不传 --config-file
SKETCH="<本包>/firmware/TaskManagerFirmware"
FQBN="esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB"
BUILD_DIR="D:/build/TaskManagerFirmware"

# 编译（--clean 很重要：规避增量编译"假绿灯"）
$PA "$AC" compile --fqbn "$FQBN" --clean --build-path "$BUILD_DIR" "$SKETCH"

# 烧录（自动探测 COM 口，或手动 ./flash.sh COM22）
$PA "$AC" upload -p COM22 --fqbn "$FQBN" --input-dir "$BUILD_DIR"
```

**编译纪律**：必须 `--clean --warnings all`，要求 0 错误 0 警告。
若遇到 `unlinkat` 报错，删除 `%LOCALAPPDATA%\arduino\sketches` 后重编（环境问题，非代码问题）。
`volatile` 自增务必写成 `x = x + 1` 形式。

> 等价地也可用 Arduino IDE 2.x 打开 `TaskManagerFirmware.ino`，板型选 ESP32S3 Dev Module / Flash 8MB，编译上传。

---

## D. 应急：免编译直接烧录预编译 bin

包内 `firmware/prebuilt/TaskManagerFirmware_R7c.bin` 是已验证的 R7c 固件（1.9 MB，应用分区镜像）。
仅当板子已有匹配的分区表/引导时可用（新板建议仍从源码烧录以确保 bootloader/partitions 一致）：

```bash
pip install esptool
esptool.py --port COM22 --baud 921600 write_flash 0x10000 \
  "<本包>/firmware/prebuilt/TaskManagerFirmware_R7c.bin"
```

> 教训：编译产物目录会被下次编译覆盖，**重要版本 bin 必须另存备份**（本包即承担了该备份角色）。

---

## E. Python 环境（tools / BLE 服务端 / 波形客户端）

`tools/`、 `电脑端工具包/`、`firmware/TaskManagerFirmware/tools/` 下的脚本依赖以下包，
用隔离 venv 安装（请勿污染系统 Python）：

```bash
python -m venv venv
venv/Scripts/pip install bleak openpyxl matplotlib numpy pyserial python-docx
```

各目录都有 `requirements.txt`（如 `电脑端工具包/requirements.txt`、`firmware/.../tools/requirements.txt`），
直接 `pip install -r requirements.txt` 即可。

常用脚本：
- `tools/selftest_rebuild.py` —— **改光电门逻辑前的必跑回归（15 场景）**。
- `tools/gate_watch.py` / `tools/diag_now.py` —— 实机振荡风暴诊断。
- `电脑端工具包/pc_web_server.py` —— 本地 Web BLE 服务端（端口 8777，双击 `启动服务.bat`）。
- `手机端工具包/ble_cli_web.html` —— 手机 Chrome Web Bluetooth 单文件调试页。

---

## F. 重印 3D 外壳

- 参数化建模：`enclosure/model_enclosure.py`（改尺寸后重生成 STL）。
- 现成 STL：`enclosure/stl/`（26 个，含整体装配 `shell_assembled.stl`）。
- 历史备份：`enclosure/stl_bak_172x72x70/`。
- 打包 zip：`enclosure/外壳STL_*.zip`（可直接发给打样厂）。
- 图纸说明：`3D打印外壳图纸（嘉立创X树脂免费打样）.html`。

---

## G. 密钥与安全（公开前必做）

源码 `firmware/TaskManagerFirmware/fw_config.h` 内含有 **私有凭据占位**，公开/上传开源仓库前必须清空：

- `WIFI_DEF_*` —— 3 条默认 WiFi（共 3 处）。
- `WX_KEY_DEFAULT` / `WX_HOST_DEFAULT` —— 和风天气 Key/Host（共 2 处）。

合计 **5 处**，缺一不可，否则会泄露个人网络与天气 API 凭据。
正常编译自用无需改动；仅在对外发布源码时清空（天气功能会回落到 BLE 配网/配 Key 流程）。

---

## H. 上电快速自检

1. 上电见桌宠 `(^o^)/` 打招呼 → 待机慢眨眼。
2. 长按切到 [2]：屏显四行字段；BLE/串口发 `2`→`2.2` 设 L=1.000。
3. 手动挡光 10 次：n/N 增长、t≈挡光间隔×2、G 收敛到 ~9.8。
4. 长按切到 [3]：发 `4`→`4.4` 配 Host/Key→`4.3` 立即更新→屏显气温。
5. 空闲 180s 自动回桌宠；5min 瞌睡→1min 熄屏；任意按键唤醒。

---

## I. 云端资料库同步（长期约定）

项目文档的改动要求**同步更新云端版本**（不只改本地）。云端节点：
`项目历程总览` = `YzeDIBoATKO9Y4XkmGa9RO`（空间 `BrgvsLCLLRy9l3W2sqBffy`），
链接 https://www.workbuddy.cn/space/d/YzeDIBoATKO9Y4XkmGa9RO

操作路径（library skill 的 `page/edit-flow.md`）：
`create_page_transaction.py` → 按返回 CDN `url` GET baseline 存 `.baseline/`
→ 改工作副本 → **双向 diff 自检**（必须剥离平台标记、只取可见文本再比）
→ `upload_page_artifact.py` → `commit_page_transaction.py --cleanup-dir`。
提交后询问是否同步到发布版本。HTML 报告需过 `page/lint_page_quality.py`（FAIL 必修）。
