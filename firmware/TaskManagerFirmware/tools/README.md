# PC 端工具集（配合固件 BLE 菜单使用）

本目录包含两个 Python 工具，均通过蓝牙（BLE NUS）与开发板 `pendulum-g` 通信。

## 公共环境要求

| 项目 | 要求 |
|---|---|
| 操作系统 | Windows 10/11、macOS 12+、主流 Linux |
| 蓝牙 | 电脑自带或外接 **BLE 4.2+** 适配器 |
| Python | **≥ 3.9**（python.org 下载，安装时勾选 Add to PATH） |

安装依赖：

```bash
pip install -r requirements.txt
```

> 不装 Python 也可用手机 BLE 调试 App（nRF Connect / LightBlue）手动收发文本，但 Excel / 绘图 / 波形功能需本工具。

---

## 工具一：ble_cli.py —— 命令行 + 数据导出

**用途**：配网、改摆长、导出历史数据（自动存 Excel + 绘 T²–L 图）

```bash
python ble_cli.py                    # 自动扫描并连接
python ble_cli.py AA:BB:CC:DD:EE:FF  # 指定地址直连
```

### 菜单

```
  1   配网（列出WiFi→选序号→输密码）
  2   摆长（查看/修改；会话内 alt <米> 改海拔）
  3   历史数据（3.1 预览 / 3.2 导出→Excel+T²-L图 / 3.3 清空）
  4   天气设置（4.1 状态 / 4.2 坐标 / 4.3 立即更新 / 4.4 Key+Host）
  0   开发板状态总览
  q   退出
```

### 历史数据导出

- 输入 `3` → `3.2`，自动生成：
  - `pendulum_history_时间戳.xlsx` — 全部测量记录
  - `pendulum_T2L_时间戳.png` — **T²–L 图**（最小二乘拟合反解 g）
- ≥2 组不同摆长数据才有拟合意义

### 常见问题

| 现象 | 处理 |
|---|---|
| 扫描不到 pendulum-g | 开发板重新上电；确认未被手机 App 占用 |
| 连接后无菜单输出 | 发送 `0` 看状态 |
| 中文乱码 | Windows 执行 `chcp 65001` 后重开 |

---

## 工具二：phyphox_stream.py —— 实时波形（phyphox 兼容）

**用途**：接收开发板推送的实时测量数据，动态绘制 T/g/Gavg 三条曲线，支持 CSV 导出。

> 这是程序2「单摆测g」的 **phyphox 兼容实时波形功能**——原版 Rust 仓库（mcitem/pendulum-gravity-measurement）内置 phyphox GATT 服务，
> 本固件在 NUS 文本协议上实现了等价功能（零新增硬件，复用现有 BLE 通道）。

```bash
python phyphox_stream.py                 # 自动扫描连接
python phyphox_stream.py AA:BB:CC:DD:EE:FF  # 指定地址
```

### 使用流程

1. 开发板上电，**长按切换到 [2] 单摆测g 程序**
2. 运行本工具，确认已连接
3. 按 **`s`** 键发送 `STREAM ON` → 开发板开始推送
4. 在开发板上按**短按键开始测量**（挡光电门）
5. matplotlib 窗口自动弹出，三条曲线实时滚动
6. 测量中随时按 **`e`** 导出当前缓冲为 CSV
7. 按 **`s`** 再次停止推送，**`q`** 退出

### 窗口说明

| 区域 | 内容 |
|---|---|
| 上图 | 周期 T 随帧序号变化（蓝线）|
| 中图 | 即时 g 值（红线，单次周期计算）|
| 下图 | 平均 Gavg 值（绿线，全部半周期累计平均）|
| 底部状态栏 | 总帧数 / 缓冲占用 / Tavg / Gavg / 数据率 / 持续时间 |

### 协议定义（#STREAM）

```
客户端 → 固件：  STREAM ON          （开始推送）
                STREAM OFF         （停止推送）

固件 → 客户端：  #STREAM BEGIN      （流开始标记）
                #STREAM <seq>,<dt_us>,<T>,<g_inst>,<g_avg>,<n>,<L>   （每帧数据）
                #STREAM END        （流结束标记：停止/断连/切程序/测量结束）
```

字段说明：

| 字段 | 类型 | 说明 |
|---|---|---|
| seq | int | 帧序号（单调递增，用于检丢帧）|
| dt_us | int | 半周期原始值（µs，相邻中点差）|
| T | float | 周期 T = 2 × halfT（秒）|
| g_inst | float | 即时 g = 4π²L/T²（m/s²）|
| g_avg | float | 平均 g = 4π²L/Tavg²（m/s²）|
| n | int | 已累计半周期数 |
| L | float | 当前摆长（m）|

### 与原版 phyphox GATT 的对照

| 维度 | 原版（Rust+Embassy）| 本固件（Arduino+NUS）|
|---|---|---|
| 传输通道 | 自定义 GATT Service（f64 notify）| NUS 文本 #STREAM（ASCII）|
| 数据格式 | 二进制 f64 LE | ASCII 逗号分隔文本 |
| 推送频率 | ISR 直接 notify | onTick 限频 实际 ≤10Hz（100ms 节拍，50ms 门限预留）|
| 手机 App | phyphox 原生 App | 本工具 PC 端 / nRF Connect 手动查看|
| 新增硬件 | 无 | 无 |
| 延迟 | <5ms | <100ms（含 NUS 排队）|

> **精度影响评估**：单摆周期 ~2s，10Hz 采样率每周期约 20 个点，对 T²–L 拟合求 g 的精度**无实质影响**。
> 接收端做**序号连续性校验**：seq 跳变时打印警告（BLE notify 无逐包 ACK，丢帧只能靠序号发现）。
> 原版二进制 GATT 的优势主要在 >100Hz 高频场景（如转速计），对本项目属于过度工程。

---

## 协议总览（开发者参考）

| 通道 | 方向 | 格式 | 用途 |
|---|---|---|---|
| NUS TX | 板→PC | `OK:` / `ERR:` / `INFO:` | 命令回包 |
| NUS TX | 板→PC | `#EXPORT BEGIN,<n>` … `#EXPORT END` | 历史批量导出 |
| NUS TX | 板→PC | `#STREAM BEGIN` … `#STREAM <csv>` … `#STREAM END` | 实时波形数据 |
| NUS RX | PC→板 | 命令词 + `\n` | 菜单控制 |

Service UUID: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
RX (写): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
TX (notify): `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

---

## 工具三：ble_cli_web.html —— 手机端蓝牙终端（ble_cli.py 网页版）

**用途**：在手机上复现 ble_cli.py 全部操作（配网/摆长/导出→Excel+T²–L 图/天气/状态），
额外内置 **#STREAM 实时波形**（T / g_inst / Gavg 三曲线 + 丢帧告警 + 波形 CSV）。
零依赖单文件，无需安装任何 App 或 Python 库（Excel 由内置迷你生成器直接产出）。

### 为什么不在 Ubuntu 容器里跑 ble_cli.py？

手机上的 Ubuntu（Operit 等方案为 chroot/proot 用户态容器）**没有蓝牙控制器访问权**：
Android 蓝牙栈独占射频芯片，容器内没有 `hci0` 也没有 BlueZ D-Bus，bleak 无法工作。
root 也无法优雅解决（需停系统蓝牙 + 内核层 hciattach，逐机型不同）。网页版改用
**Chrome Web Bluetooth** 直接调用 Android 系统蓝牙栈，彻底绕开该限制。

可在 Operit 终端里自检验证（预期都失败）：

```bash
ls /sys/class/bluetooth        # 预期输出为空（无 hci0）
apt install -y bluez && hciconfig   # 预期无任何设备
```

### 启动步骤（HyperOS + Operit Ubuntu）

1. 把本 `tools/` 目录放进手机存储（Operit 工作区即可）
2. Operit 终端（Ubuntu）里：
   ```bash
   cd /path/to/tools
   python3 -m http.server 8000
   ```
3. 手机 **Chrome / Edge**（Chromium 内核必需，小米自带浏览器不支持 Web Bluetooth）打开：
   ```
   http://127.0.0.1:8000/ble_cli_web.html
   ```

> Web Bluetooth 仅在 `https` 或 `localhost` 安全上下文可用，**直接点开 HTML 文件（file://）不行**，
> 必须经上面的本地服务打开。下载的文件在手机「下载/Download」目录。

### 操作对照（与 ble_cli.py 完全一致）

| PC 版操作 | 网页版 |
|---|---|
| 菜单 0/1/2/3/4、q | 终端输入或快捷按钮 |
| 导出 `3.2` → xlsx + T²–L 图 | 自动收数（20s 看门狗降级同 PC）→ 自动下载 Excel，按钮可补下 CSV / PNG |
| 摆长 2 → 2.2 → 值 → y | 终端照提示输入 |
| 海拔 `alt 500` | 终端输入（摆长会话内） |
| `SIM ON/OFF`、`APP 1/2/3` | 快捷按钮 |
| phyphox_stream.py 波形 | 「实时波形」卡片：STREAM ON/OFF + 三曲线 + 丢帧告警 + CSV 导出（息屏断流，页面自动申请屏幕常亮） |

### 常见问题

| 现象 | 处理 |
|---|---|
| 打开页面无蓝牙权限提示 | 换 Chrome；确认系统设置里浏览器蓝牙权限已允许 |
| 扫不到 pendulum-g | 板子重新上电；确认未被 nRF Connect 等占用 |
| 波形时卡时断 | 保持页面前台；给浏览器设「无限制」省电策略 |
| Excel 下载失败 | 用「下载 CSV」按钮兜底（WPS/Excel 均可打开） |

---

## 工具四：pc_web_server.py —— 电脑端 WebUI（与手机端同一套界面）

**用途**：把 ble_cli.py + phyphox_stream.py 的能力搬到浏览器界面。
前端**复用同一个 `ble_cli_web.html`**，差别只在传输层：

| 模式 | 触发条件 | 蓝牙由谁负责 |
|---|---|---|
| 后端模式 | 页面由 `pc_web_server.py` 提供（`/api/ping` 有响应） | 本机 Python bleak |
| Web Bluetooth 模式 | 页面由 `python3 -m http.server` 提供（手机） | 浏览器 → 系统蓝牙栈 |

```bash
pip install bleak            # 本工具实际只需 bleak
python pc_web_server.py      # 浏览器自动打开 http://127.0.0.1:8777/
python pc_web_server.py AA:BB:CC:DD:EE:FF --port 9000
```

Windows 可直接双击 `启动服务.bat`（自动装依赖、自动开浏览器）。

### 实现要点

- **零 Web 框架**：asyncio 手写 HTTP + SSE（`/api/events` 推送、`/api/send` 发送），
  除 bleak 外无新依赖；Excel/绘图仍在浏览器侧完成（故手机也能跑同一界面）
- **行缓冲**：notify 可能半行到达，服务端按行缓冲后再推送，前端二次按行解析（双保险）
- **断线自愈**：bleak 回调线程 → `call_soon_threadsafe` 广播；页面 EventSource 自动重连
- **慢客户端保护**：SSE 队列满时丢最旧帧，不阻塞 BLE 回调
- **服务自检**：`POST /api/selftest` 注入脚本化「菜单→60 帧 #STREAM→#EXPORT 3 组」，
  无硬件即可验证终端/波形/表格/T²–L 图/Excel 下载整条链路
  （注入数据按 T=2π√(L/g) 构造且物理自洽，拟合 g≈9.8）

### 已验证（真实 Edge 无头浏览器 + CDP 驱动）

`/api/ping`、页面 36.9KB、SSE 72 事件（含 60 帧波形与 EXPORT 块）；页面自动识别后端模式 →
点「服务自检」→ 导出状态"完成"、表格 3 行、T²–L 图绘制（k=4.0363, g≈9.781）、
波形 60 帧丢帧 0、Excel 自动下载。截图见 `pc_webui_selftest.png`。

## 工具五：串口命令验证（serial_cmd_verify.py / wx_verify.py）

本机 Windows 蓝牙栈对该设备 GATT 不兼容（连上即断；手机 nRF Connect 可稳定连），
故固件新增**串口命令通道**：COM22 @115200，发任意 BLE 命令词 + `\n` 即可，
回复带 `[BLE>]` 前缀。

```bash
python serial_cmd_verify.py   # 全功能回归：状态/天气/单摆/altstat/SIM 数值校验
python wx_verify.py           # 天气专项：切程序3 → 立即更新 → 观察拉取全过程
```

### 使用要点

- **开串口会复位板子**（`rst:0x15 USB_UART_CHIP_RESET`），属正常现象；命令需在复位后重新下发
- **天气菜单是一次性的**：执行任一子项即 `endSession`，故每条子命令都要先发 `4` 进菜单
- `APP 1|2|3` 切程序（走 Manager 正规生命周期）；`SIM <半周期秒>` 注入已知数据做数值自检
- `altstat` 输出快慢交替诊断（交替是否检出/幅度/次数/n/参与/落单排除/G/T）

### 已验证结果（2026-10-02 上板）

- 串口命令全通：`[0]` 状态 / `APP n` 切程序 / `[2]` `[4]` 菜单 / `altstat` / `SIM`
- 配对结算数值精确：`SIM 0.5` → `n=35 参与=34 落单排除=0 T=1.0000s`（理论 T=1s，误差 0）
- 天气：`Key=已配置 Host=已配置`（内置凭据生效，无需先配网）；
  但内置 Host 区域码 `free` 错误 → 正确报 `-105 DNS 解析失败`（需到控制台核对 API Host）

## 工具六：inflate 参考实现验证器（inflate_ref.py）

`fw_inflate.cpp`（自带 DEFLATE/gzip 解压）的**参考实现与验证器**——C++ 版移植自它。
日后修改 inflate 逻辑，请先在此验证正确性再同步。

```bash
python inflate_ref.py --self              # 内置用例自检（8/8）
python inflate_ref.py <file.gz> [...]     # 验证指定 gzip 文件
```

覆盖 stored / fixed-Huffman / dynamic-Huffman 三种块类型、空内容、长距离码回溯，
对照 `zlib.decompress` 逐字节比对并校验 CRC32；截断/损坏数据应报错而非静默输出。

**两个必须记住的坑**（都曾真实踩到）：

1. CRC32/ISIZE 要从**缓冲区末尾**读 8 字节，不能用位读取器结束时的 `pos`
   （末字节未用尽会偏 2 字节 → CRC 误判）
2. 距离匹配必须**逐字节**拷贝，不能整段 `memcpy`（相邻匹配会自我重叠，如 `dist=1`）

## 工具七：重建层纠错算法离线仿真（selftest_rebuild.py）

**用途**：改动 `app2_pendulum.cpp` 的重建/纠错逻辑后，**先跑这个再刷机**。
它复刻固件 `onTick` 的全部判据与算术（同一套门限、同一套公式），
用可复现的输入验证「恢复有效、删除有效、且不误伤」。

```bash
python selftest_rebuild.py
```

覆盖 9 个场景 / 25 项断言：理想输入、系统性丢样、偶发丢沿、一次过零被抖成多片、
相邻遮挡贴太近、电气毛刺、离群点、摆长填错、混合 torture。

**这个工具已经抓出过一个真实设计缺陷，值得记住**：

初版有个「k 个遮挡粘成一块 → 分裂」的恢复逻辑，判据是「块长 ≈ k×块宽」。
仿真里为了让这条分支跑过，用 `dur = 2×blk + h` 造了数据 —— 但那组数据在真实硬件上
**不可能出现**：两次真实过零相隔一个半周期（本例 1.0 s），中间高电平就有 0.98 s，
传感器不可能分辨不出，LOW 段根本不会因这个原因连成一片。
也就是说那是一段**永远不触发的死代码**，而仿真数据是「为让测试通过而设计的假象」。

正确结论：真实的漏记只有一种 —— 传感器漏掉整个跳变沿，表现为间隔变 2h/3h，
由**整数倍恢复**处理；而 LOW 段裂成几片是「一次过零被抖成多片」，
应当**合并成 1 个中点**而不是分裂成 k 个。两条路都已在固件与仿真中改正。

> 教训：仿真的价值不在于「让测试通过」，而在于逼出「我的数据在物理上可能出现吗」这个提问。
