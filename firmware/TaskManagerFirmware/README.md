# TaskManagerFirmware —— 多程序固件（总程序 + 桌宠 + 单摆测g + 天气）

依据《00-总纲》《01-桌宠》《02-单摆测g》《03-天气》四份规范实现，**三个程序全部完成**：
程序1 桌宠屏保（默认·home）｜程序2 单摆测g｜程序3 实时天气（和风 QWeather）。

## 1. 环境要求

| 项 | 版本/说明 |
|---|---|
| Arduino IDE | 2.x |
| esp32 板卡包 | **3.x**（Espressif 官方） |
| 板型 | ESP32S3 Dev Module，Flash 8MB |
| 第三方库 | **U8g2**（库管理器安装；固件含 wqy 中文/GB2312 字体，无需额外中文字库） |
| 内置库 | WiFi / BLE / Preferences / HTTPClient / Wire |

> 中文显示使用 U8g2 自带 `wqy12/wqy16` GB2312 字体（UTF-8 输入），Flash 占用约 +0.5MB，8MB 板载无压力。

## 2. 文件结构

```
TaskManagerFirmware/
├── TaskManagerFirmware.ino   主入口
├── fw_config.h               引脚/时序/错误码常量
├── fw_types.h                App 生命周期契约 + 事件总线
├── fw_store.h/.cpp           存储服务（NVS 键值 + 50 条环形历史）
├── fw_time.h/.cpp            时间服务（SNTP ntp.aliyun.com GMT+8）
├── fw_display.h/.cpp         显示服务（SH1106/U8g2、独占授权、通知条、L3 熄屏）
├── fw_net.h/.cpp             网络服务（多凭据回退、懒连接、空闲 5min 断开、HTTP+GZIP）
├── fw_ble.h/.cpp             BLE NUS 透传 + 菜单路由 + 配网/应用会话 + #STREAM 流
├── fw_manager.h/.cpp         程序管理器（按键路由/屏保/生命周期分发）
├── app1_pet.h/.cpp           程序1 桌宠（表情状态机/事件联动/熄屏协作/AI 桩）
├── app2_pendulum.h/.cpp      程序2 单摆测g（中断计时/四行显示/历史/导出/**实时波形**）
├── app3_weather.h/.cpp       程序3 天气（QWeather/四屏/缓存降级）
├── docs/                     **每程序独立说明文档**
│   ├── README.md             文档索引
│   ├── 程序1-桌宠.md          桌宠屏保详解
│   ├── 程序2-单摆测g.md       单摆测g详解（含 #STREAM 协议定义）
│   └── 程序3-天气.md          实时天气详解
└── tools/                    PC 端配套工具
    ├── ble_cli.py             命令行 + Excel 导出 + T²–L 图
    ├── phyphox_stream.py      **实时波形客户端（matplotlib 动态绘图）**
    ├── requirements.txt       Python 依赖
    └── README.md              工具使用说明
```

> 命名说明：总纲 §4 目录示例与 §7 NVS 键总表编号冲突，本实现以 NVS 表为准：app1=桌宠、app2=单摆、app3=天气。

## 3. 按键操作（GPIO5）

| 动作 | 效果 |
|---|---|
| 短按 | 转发当前程序：桌宠=互动换表情；单摆=开始/停止测量；天气=四屏切换 |
| 按住 ≥1.2s | 切换程序（1桌宠→2单摆→3天气→循环） |
| 按住 ≥5s | 系统重启 |
| 熄屏后任意按键 | 仅唤醒屏幕（当次不触发主功能） |

## 4. 程序2 单摆测g 要点（分册02）

- **中点法计时**（分册02修订补丁）：GPIO4 中断 CHANGE 双沿，取遮挡时段中点为计时点，相邻中点差=半周期；遮挡 <20ms 毛刺整对丢弃、中点差 >5s 重置基准；`micros()` 计时，临界区读取
- **ISR SPSC 环形队列（16 槽）**：半周期样本入队而非只保留「最近一个」，主循环每次 tick 排空队列。原实现在 tick 被 OLED 整帧提交拉长时会静默丢样本（短摆 L=0.1m 半周期仅 ~0.32s），导致半周期数偏少、Tavg 略偏。显示字段 `n` 与 `N` 现已同源（均为已消费样本数）
- OLED 四行布局字段与原版一致：`g/n/t/L/sG/N/G`，角标 `err` + [测量中]/[待机] + sG 数据年龄
- sG 按国际重力公式本地计算（纬度取 `sys.loc_lat`，海拔 `app2.alt_m` 默认 0），写缓存 sg_src=CALC；无缓存默认 9.794
- 测量结束自动写历史（环形 50 组）并广播 EVT_MEASURE_DONE（桌宠庆祝联动）
- **长按切走再切回可续测**（测量累计值保留 RAM）；断电恢复 L/历史/sG
- BLE [2] 摆长：2.1 查看 / 2.2 修改（0.1–5.0 m，二次确认 y/n）/ 会话内隐藏命令 `alt 123.4` 改海拔（-500~9000 m，改后立即重算 sG）／任意会话状态键入 `q` 退出
- BLE [2.4] 实时波形：`STREAM ON` / `STREAM OFF` 顶层直发（无需先进会话），或在 `[2]` 内走 `[2.4]` 切换；推送条件 = 程序2活跃 + 测量中 + BLE已连；实际限频 ≤10Hz（onTick 100ms 节拍，50ms 门限预留）；测量结束/切程序/BLE断连自动停止
- BLE [3] 历史：3.1 预览 / 3.2 导出全部（`#EXPORT BEGIN,<count>` … `#EXPORT END`，分块发送不断流）/ 3.3 清空（键入 CLEAR）
- **PC 端配套工具**：
  - `tools/ble_cli.py`（配网/改摆长/导出历史→自动存 Excel+绘 T²–L 图并反解 g）
  - `tools/phyphox_stream.py`（**实时波形**：动态绘制 T/g/Gavg 三曲线+运行统计+CSV 导出，phyphox 兼容）
  - 环境与用法见 `tools/README.md` + `tools/requirements.txt`（需 Python ≥3.9 + 蓝牙适配器）
- 光电门极性相反（挡光=高）时把 `gateISR` 的判据 LOW/HIGH 对调

## 5. 程序3 天气要点（分册03）

- 数据源和风 QWeather v7：`/v7/weather/now` + `/v7/weather/24h`，30 分钟周期；坐标**经度在前**；响应 GZIP 已内置解压
- **自动重连**：程序3待拉取且 WiFi 未连接时自动发起连接（30s 间隔防振荡）——重启后无需手动配网即可恢复拉取/对时
- **多凭据自动回退**：凭据列表 = NVS 用户配网凭据（BLE [1.2] 设置，永远首选）+ `fw_config.h` 内置默认回退列表（**开源版已清空为占位，请填入你自己的网络**）。连接失败（AP 不在范围/认证失败/20s 超时）自动轮换下一个。
- **wx_hourly 为紧凑格式** `fxTime|temp|pop;…`（约600B；NVS 单值约 4KB 上限装不下原始 24h JSON），解析见 app3_weather.cpp
- 凭据**可内置于固件**（`fw_config.h` 的 `WX_KEY_DEFAULT`/`WX_HOST_DEFAULT`）：填入后出厂通电即可拉天气，**无需先配 Key**。优先级 NVS（BLE [4]→[4.4] 覆盖）> 内置值；`app3_weather.cpp` 另有兜底，NVS 被清空时回落内置。**开源版这 2 处与 `WIFI_DEF_*` 共 5 处均已清空为占位，请填入你自己的凭据。**
- 四屏：①气温大字 ②天气现象+点阵图标（晴/云/雨/雪/雾）+今日最高/最低 ③风速风向 ④未来 6h 降水竖条点阵+开始时间（pop≥30%）
- 每屏角标「HH:MM 数据」；>24h 追加 `!`；拉取失败不切屏只更新角标，30s 后重试 1 次
- 无缓存首屏为等待页；断网全程走缓存
- BLE [4]：4.1 状态 / 4.2 改坐标（校验经度 73–135、纬度 3–54）/ 4.3 立即更新 / 4.4 配 Key+Host

## 6. BLE 使用（广播名 `pendulum-g`，NUS）

- Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`；写 `…0002`；Notify `…0003`
- nRF Connect / LightBlue / WebBLE 发送命令词 + `\n`
- 菜单总表：`[0]` 状态总览 `[1]` WiFi 配网 `[2]` 摆长/波形 `[3]` 历史 `[4]` 天气 `[5]` AI 桩；发 `HELP` 列出
- **PC 断连会自动复位所有交互会话**（含各程序内部会话状态机，走 `App::onBleAbort()`）。未复位时重连后的第一条命令会被残留状态解释（例如再发 `2` 直接跳进摆长输入态而不打印菜单）
- **实时波形协议**（程序2，phyphox 兼容）：
  - 客户端发送 `STREAM ON` / `STREAM OFF`（或 BLE 菜单 `[2]` → `[2.4]`）
  - 固件推送 `#STREAM BEGIN` → `#STREAM <seq>,<dt_us>,<T>,<g_inst>,<g_avg>,<n>,<L>` → `#STREAM END`
  - PC 客户端 `tools/phyphox_stream.py` 自动绘图；详见 `tools/README.md` §工具二

## 6.1 串口命令通道（2026-10-02 新增）

USB CDC（COM22，115200）可发**与 BLE 完全相同的命令**（行尾 `\n`）：

```
[CLI<] 0          ← 收到的命令回显
[BLE>] INFO: ...  ← 回复（前缀与 BLE 一致）
[CLI] 残缺行已丢弃: ...   ← 4s 无换行的看门狗提示
```

- 实现：`cliSerialPump()` 逐字节**非阻塞**读串口 → `Ble.pushSerialLine()` 入 BLE 同一行队列 → `tick()` 走既有路由。故 `[0]`/`[2]`/`[4]`/`APP n`/配网会话全部通用，回复自动落串口
- 由来：本机 Windows 蓝牙栈对该设备 GATT 不兼容（连上即断，手机 nRF Connect 可稳定连），串口是唯一可靠通道
- PC 端脚本：`tools/serial_cmd_verify.py`（自动下发命令并收集回复）
- ⚠️ 打开串口会触发 `rst:0x15 (USB_UART_CHIP_RESET)` 复位，属正常现象；命令需在复位后重新下发
- 抗噪：非可打印字节直接丢弃整行，行长上限 120 字符，队列上限 16 行

## 6.2 zlib 缺失与自带 inflate（2026-10-02）

**问题**：本项目工具链 `esp32:esp32 3.3.12-cn` **不含 zlib**（`packages/`、`tools/` 全盘搜索
无 `zlib.h` / `libz.a`），而和风天气**无视 `Accept-Encoding: identity`，强制返回 GZIP**，
天气拉取必然失败（`E_NET_GZIP = -102`）。GitHub 直连与 gh-proxy 镜像均限流，装不了官方 Zlib 库。

**解决**：新增 `fw_inflate.h/.cpp` —— 自带 DEFLATE（RFC 1951）+ gzip（RFC 1952）解压，
含 CRC32 自实现。支持 stored / fixed-Huffman / dynamic-Huffman 三种块类型。

**正确性验证**：`tools/inflate_ref.py`（参考实现 + 验证器，`fw_inflate.cpp` 移植自它）

```bash
python inflate_ref.py --self     # 8/8 全过，CRC32 全部一致
```

覆盖：真实 QWeather now/24h 响应、高压缩数据（fixed）、随机数据（dynamic）、
类 JSON 重复文本（长距离码回溯）、小数据（stored）、空内容、超长重复序列；
边界用例（截断/损坏）正确报错而非静默输出。

**两个必须记住的坑**

1. **CRC32/ISIZE 必须从缓冲区末尾读 8 字节**，不能用位读取器结束时的 `pos`——
   末字节往往未用尽，`pos` 会偏 2 字节，导致 CRC 误判不通过（上板实测踩到，`5d0b6606 != 5e5d0b66`）。
2. **距离匹配必须逐字节拷贝**，不能整段 `memcpy`——相邻匹配会自我重叠（如 `dist=1`）。

**运行结果**：Flash 56%（自带 inflate 约 +3 KB）/ RAM 16%。
上板实测 `httpGet OK: 200, 449 bytes` → `[Wx] update OK` → 天气四屏数据正常。

## 7. 已知取舍（与冻结设计的偏差说明）

1. **httpGet 为阻塞式**（每请求最长 10s）：程序3 的 30 分钟拉取会在其活跃时阻塞 UI 约 5–15s；因总纲禁止程序自建 FreeRTOS 任务，此为工程折中。拉取仅发生在程序3活跃时（非活跃程序零阻塞，比"后台拉取"更保守）。
2. **历史导出分块发送**（每 tick 8 行）：避免 50 组一次性发送违反 50ms 铁律。导出/实时波形期间 `isBusy()` 返回 true，豁免 L0 屏保；若用户在导出中途长按切换程序，`onHide` 会立即补发 `#EXPORT END` 收尾（PC 端 `ble_cli.py` 按已收数据正常落盘并提示中断），不会再出现 `#EXPORT END` 永不到达的双端卡死。`ble_cli.py` 另有 20s 导出看门狗：固件侧异常断电等收不到 END 的场景，按部分数据降级保存。
3. **实时波形限频 ≤10Hz**：onTick（100ms 节拍）中推送，避免 NUS 通道拥塞影响菜单响应；单摆周期 ~2s 时每周期约 20 个采样点，对 g 计算精度无实质影响。
4. **#STREAM 为 NUS 文本协议**（非原生 phyphox GATT 二进制）：原版 Rust 仓库用自定义 GATT Service + f64 notify，延迟 <5ms；本方案复用现有 NUS 通道，延迟 <100ms，PC 端需专用客户端解析。权衡点：零新增硬件/零新增 GATT Service/手机 nRF Connect 可手动查看原始数据流。
5. 颜文字为 ASCII 字形（分册允许微调）；天气现象中文用 wqy GB2312 字体正常显示。
6. 管理 [0] 状态中的"剩余 NVS"读自 nvs_get_stats（已用/剩余条目数）。
7. **L2 空闲断电会广播 `EVT_NET_LOST`（`code=EVT_NET_LOST_CODE_POWERSAVE=1`）**，程序1 据此区分「主动省电」与「意外掉线」——否则 5 分钟一次的常规断电会让桌宠永久保持 Sad 表情，看起来像一直在报错。
8. **WiFi 扫描（`[1.2]`）结束后会立刻关闭射频**（SSID 列表已缓存到本地，`scanDelete()` 后不再需要射频）。扫描被 `q` 取消同理。

## 8. 烧录后快速自检

1. 上电即见桌宠 `(^o^)/` 打招呼 → 待机慢眨眼（不记忆上次程序）
2. 长按切到 [2]：屏显四行字段；BLE 发 `2`→`2.2` 设 L=1.000
3. 手动挡光 10 次：n/N 增长、t≈挡光间隔×2、G 出现
4. 长按切到 [3]：BLE 发 `4`→`4.4` 配 Host/Key→`4.3` 立即更新→屏显气温
5. 空闲 180s 自动回桌宠；5min 瞌睡→1min 熄屏；任意按键唤醒
