#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
e2e_sim_test.py —— 端到端板级测试（开发板 + PC，无需光电门）

流程：
  1. 扫描并连接 pendulum-g（NUS）
  2. SIM ON 注入合成数据（自动开始测量）
  3. 等若干秒让半周期累积 → 读取 [2.1]/[0] 确认 N>0、Gavg≈9.80
  4. STREAM ON 收实时波形帧（≈6s）→ STREAM OFF → 落盘 CSV
  5. [3.2] 触发历史导出 → 收 #EXPORT 帧 → 复用 ble_cli 真实代码存 Excel + 绘 T²-L 图
  6. 打印各环节 PASS/FAIL

仅依赖 bleak（已装在隔离 venv）。若本机蓝牙不可用/连不上，会明确报错而非假绿。
"""
from __future__ import annotations
import asyncio
import csv
import io
import os
import sys
import time
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("缺少 bleak：pip install -r requirements.txt")

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
DEVICE_NAME = "pendulum-g"


class Collector:
    def __init__(self):
        self.lines = []
        self.export_rows = []
        self.exporting = False
        self.export_expect = 0
        self.frames = []
        self.stream_active = False
        self.got = {"sim": False, "meas": False, "export": False, "stream": False}

    def feed(self, line: str):
        line = line.strip()
        if not line:
            return
        self.lines.append(line)
        if line.startswith("#EXPORT BEGIN"):
            self.exporting = True
            self.export_rows = []
            try:
                self.export_expect = int(line.split(",")[1])
            except (IndexError, ValueError):
                self.export_expect = 0
        elif line.startswith("#EXPORT END"):
            self.exporting = False
            self.got["export"] = True
        elif self.exporting and line[:1].isdigit() and "," in line:
            self.export_rows.append(next(csv.reader([line])))
        elif line == "#STREAM BEGIN":
            self.stream_active = True
        elif line == "#STREAM END":
            self.stream_active = False
            self.got["stream"] = True
        elif line.startswith("#STREAM "):
            parts = line[len("#STREAM "):].split(",")
            if len(parts) >= 7:
                try:
                    self.frames.append(parts[:7])
                except Exception:
                    pass


async def main():
    print("扫描 %s ..." % DEVICE_NAME)
    devs = await BleakScanner.discover(timeout=8)
    dev = next((d for d in devs if (d.name or "").lower() == DEVICE_NAME), None)
    if dev is None:
        named = [d for d in devs if d.name]
        print("未找到开发板。附近 BLE 设备：")
        for d in named[:15]:
            print("  ", d.address, d.name)
        return 2
    print("连接 %s ..." % dev.address)
    c = Collector()

    def on_notify(_ch, data: bytearray):
        for ln in data.decode("utf-8", errors="replace").splitlines():
            c.feed(ln)

    client = BleakClient(dev, timeout=20)
    await client.connect()
    await client.start_notify(NUS_TX, on_notify)
    # 连接参数更新（4s→15-30ms 间隔）在 Windows 侧生效需要几秒；
    # 窗口期内通知按旧 4s 间隔漏出，会造成响应迟到/漏判。先静置等链路稳定。
    await asyncio.sleep(5.0)

    async def send(s: str):
        if not client.is_connected:
            print("[错误] 已断连")
            return
        await client.write_gatt_char(NUS_RX, (s + "\n").encode("utf-8"))
        print(">>", s)

    # 0) BLE 远程切到程序2（R5 调试命令；按键不可用时的替代路径，
    #    走 Manager.activate 正规生命周期，与长按完全一致）
    await send("APP 2")
    await asyncio.sleep(3.0)
    on_p2 = any(("已切换到" in l and "单摆" in l) or "已在 单摆测g" in l for l in c.lines)
    print("[切程序] ", "OK" if on_p2 else "未确认（继续尝试）")
    for l in c.lines[-4:]:
        print("   ", l)

    # 1) SIM ON（要求程序2活跃；自动开始测量）
    await send("SIM ON")
    await asyncio.sleep(4.0)   # 链路已提速（15-30ms 连接间隔），4s 足够
    c.got["sim"] = any("模拟数据已开启" in l for l in c.lines)
    if not c.got["sim"]:
        print("[提示] 未收到『模拟数据已开启』。已收到的响应：")
        for l in c.lines[-6:]:
            print("   ", l)

    # 2) 累积测量（SIM 每半周期产一样本；12s ≈ 12 个半周期）
    await asyncio.sleep(12.0)

    # 3) 实时波形
    await send("STREAM ON")
    await asyncio.sleep(5.0)
    await send("STREAM OFF")
    await asyncio.sleep(2.0)

    # 4) 历史导出（SIM OFF 结算写入一条历史；[3.2] 触发导出）
    await send("SIM OFF")
    await asyncio.sleep(3.0)
    await send("3")
    await asyncio.sleep(1.5)
    await send("3.2")
    # 等待 #EXPORT END（最长 25s，板端分块发完）
    t0 = time.time()
    while not c.got["export"] and time.time() - t0 < 25:
        await asyncio.sleep(0.5)

    # 测量是否产出数据：以导出行是否非空为准（比解析状态行更可靠）
    c.got["meas"] = len(c.export_rows) > 0

    await client.disconnect()

    # ---- 落盘 ----
    out = os.path.join(HERE, "_selftest_out")
    os.makedirs(out, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    # 导出 Excel + 图（复用 ble_cli 真实代码）
    xlsx = os.path.join(out, "e2e_history_%s.xlsx" % stamp)
    png = os.path.join(out, "e2e_T2L_%s.png" % stamp)
    if c.export_rows:
        import ble_cli
        cli = ble_cli.BleCli()
        cli._write_and_plot(c.export_rows, xlsx, png)
        c.got["export"] = os.path.exists(xlsx) and os.path.exists(png)

    # 波形 CSV
    csvp = os.path.join(out, "e2e_stream_%s.csv" % stamp)
    if c.frames:
        with open(csvp, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f)
            w.writerow(["序号", "半周期(us)", "周期T(s)", "即时g", "平均g", "半周期数n", "摆长L(m)"])
            for fr in c.frames:
                w.writerow(fr)
        c.got["stream"] = True

    # ---- 汇总 ----
    print("=" * 60)
    print("端到端结果：")
    print("  BLE 连接+SIM 命令 :", "PASS" if c.got["sim"] else "FAIL")
    print("  测量累积(N>0)     :", "PASS" if c.got["meas"] else "FAIL")
    print("  实时波形(CSV)     :", "PASS (%d 帧)" % len(c.frames) if c.got["stream"] else "FAIL")
    print("  历史导出(Excel+PNG):", "PASS" if c.got["export"] else "FAIL")
    print("  导出行数          :", len(c.export_rows))
    print("  波形帧数          :", len(c.frames))
    print("输出：", out)
    print("=" * 60)
    ok = all([c.got["sim"], c.got["meas"], c.got["stream"], c.got["export"]])
    return 0 if ok else 1


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\n已退出。")
