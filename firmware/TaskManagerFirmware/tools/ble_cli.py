#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ble_cli.py —— 单摆测g 电脑端蓝牙命令行（副1-3 配套工具）

功能（对应开发板 BLE 菜单）：
  [1] 为开发板配网：列出扫描到的 WiFi 供选择，输入密码写入 NVS
  [2] 查看/修改摆长（0.1–5.0 m，二次确认），会话内可改海拔 alt
  [3] 历史数据：预览 / 导出全部（自动保存 Excel + 绘制 T²–L 图）/ 清空
  [4] 天气设置（状态 / 坐标 / 立即更新 / Key+Host）
  [0] 开发板状态总览

用法：
  python ble_cli.py                 # 自动扫描名为 pendulum-g 的开发板
  python ble_cli.py AA:BB:CC:DD:EE:FF   # 指定地址直连

依赖见同目录 requirements.txt；Python 需 ≥ 3.9。
导出流协议：#EXPORT BEGIN,<count> … 数据行 … #EXPORT END（与固件 fw_ble 一致）。
"""

from __future__ import annotations

import asyncio
import csv
import io
import math
import os
import sys
import threading
from datetime import datetime

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("缺少 bleak 库：请先执行  pip install -r requirements.txt")

# ---------- NUS（Nordic UART Service），与固件一致 ----------
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # PC→板（写）
NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # 板→PC（notify）

DEVICE_NAME = "pendulum-g"

HELP_TEXT = """
==================== 单摆测g · 蓝牙命令行 ====================
  1   配网（列出WiFi→选序号→输密码）
  2   摆长（查看/修改；会话内 alt <米> 改海拔）
  3   历史数据（3.1 预览 / 3.2 导出→Excel+T²-L图 / 3.3 清空）
  4   天气设置（4.1 状态 / 4.2 坐标 / 4.3 立即更新 / 4.4 Key+Host）
  0   开发板状态总览
  q   退出本工具（开发板连接保持直至断开）
  ------------------------------------------------------------
  会话中直接输入开发板提示的内容；q 可退出当前会话。
==============================================================
"""

# 导出数据行格式（固件 finalizeMeasure）：
#   序号,YYYY-MM-DD HH:MM,L,N,Tavg,Gavg,err%
EXPORT_COLS = ["序号", "时间", "L(m)", "N(半周期数)", "Tavg(s)", "Gavg(m/s²)", "相对误差"]


class BleCli:
    def __init__(self, address=None):
        self.address = address
        self.client = None
        self.exporting = False
        self.export_expect = 0
        self.export_rows: list[list[str]] = []
        self._wd: threading.Timer | None = None   # 导出看门狗（见 _arm_export_watchdog）

    # ---------------- 连接与收发 ----------------
    async def connect(self):
        if self.address:
            dev = await BleakScanner.find_device_by_address(self.address, timeout=10)
        else:
            print(f"正在扫描 {DEVICE_NAME} …（约 5 秒）")
            devs = await BleakScanner.discover(timeout=5)
            dev = next((d for d in devs if (d.name or "").lower() == DEVICE_NAME), None)
            if dev is None:
                named = [d for d in devs if d.name]
                print("未找到开发板。附近可见 BLE 设备：")
                for d in named[:15]:
                    print(f"  {d.address}  {d.name}")
                sys.exit("请确认开发板已上电、未被手机 App 占用，或用地址直连。")
        print(f"连接 {dev.address} ({dev.name}) …")
        self.client = BleakClient(dev, disconnected_callback=self._on_disconnect)
        await self.client.connect()
        await self.client.start_notify(NUS_TX, self._on_notify)
        print("已连接。")
        print(HELP_TEXT)

    def _on_disconnect(self, _):
        # 注意：本回调运行在 bleak 的回调线程里。原来在这里调 sys.exit() 只会
        # 终止该线程，主线程仍阻塞在 input() 上，表现为"窗口假死、必须 Ctrl-C"。
        # CLI 工具的语义下，开发板消失就应当整体退出，故显式结束进程。
        print("\n[连接已断开] 请检查开发板电源后重启本工具。")
        try:
            sys.stdout.flush()
        finally:
            os._exit(1)

    def _on_notify(self, _ch, data: bytearray):
        text = data.decode("utf-8", errors="replace")
        for line in text.splitlines():
            self._handle_line(line.strip())

    def _handle_line(self, line: str):
        if not line:
            return
        if line.startswith("#EXPORT BEGIN"):
            self.exporting = True
            self.export_rows = []
            try:
                self.export_expect = int(line.split(",")[1])
            except (IndexError, ValueError):
                self.export_expect = 0
            print(f"\n[导出开始] 预计 {self.export_expect} 组，请勿关闭窗口…")
            self._arm_export_watchdog()
            return
        if line.startswith("#EXPORT END"):
            was_exporting = self.exporting
            self._cancel_export_watchdog()
            self.exporting = False
            if not was_exporting and not self.export_rows:
                return                      # 看门狗已收尾过，迟到的 END 不再重复落盘
            print(f"[导出结束] 实收 {len(self.export_rows)} 组")
            self._save_export()
            return
        if self.exporting and line[0:1].isdigit() and "," in line:
            self.export_rows.append(next(csv.reader([line])))
            print(f"  接收 {len(self.export_rows)}/{self.export_expect}: {line}")
            self._arm_export_watchdog()     # 每收到一行重置看门狗
            return
        print(f"  {line}")            # OK:/ERR:/INFO:/菜单等直接显示

    # ---------------- 导出看门狗 ----------------
    # 固件侧导出是分块推送：正常时每 100ms 就有一批。若 20 秒无一字
    # （板子断电/重启/异常中断且 END 丢失），继续等永远不会结束——
    # 而本工具在导出期间忽略键盘输入，表现为整体卡死。看门狗到点强制
    # 结束并按已收到的数据落盘，保证至少拿到部分结果。
    WATCHDOG_S = 20.0

    def _arm_export_watchdog(self):
        self._cancel_export_watchdog()
        self._wd = threading.Timer(self.WATCHDOG_S, self._export_watchdog_fire)
        self._wd.daemon = True
        self._wd.start()

    def _cancel_export_watchdog(self):
        if self._wd is not None:
            self._wd.cancel()
            self._wd = None

    def _export_watchdog_fire(self):
        if not self.exporting:
            return
        self.exporting = False
        print(f"\n[警告] 导出流 {self.WATCHDOG_S:.0f} 秒无新数据，判定固件侧已中断。")
        print(f"[降级保存] 已收 {len(self.export_rows)}/{self.export_expect} 组，按部分数据落盘。")
        self._save_export()

    async def send(self, msg: str):
        if self.client is None or not self.client.is_connected:
            print("未连接（可能已断开，请重启本工具）。")
            return
        try:
            await self.client.write_gatt_char(NUS_RX, (msg + "\n").encode("utf-8"))
        except Exception as e:
            # 链路异常（板子刚断电等）：不抛出崩溃，提示后继续由断连回调收尾
            print(f"[错误] 发送失败：{e}")

    # ---------------- 导出落盘 ----------------
    def _save_export(self):
        if not self.export_rows:
            print("无数据可保存。")
            self.export_rows = []
            return
        rows = self.export_rows
        self.export_rows = []       # 先取走：重复调用（迟到 END）不会再落盘第二份
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        xlsx = f"pendulum_history_{stamp}.xlsx"
        png = f"pendulum_T2L_{stamp}.png"
        self._write_and_plot(rows, xlsx, png)

    def _write_and_plot(self, rows: list[list[str]], xlsx: str, png: str):
        try:
            self._write_xlsx(rows, xlsx)
        except Exception as e:      # Excel 失败不影响绘图
            print(f"[警告] Excel 保存失败：{e}")
        try:
            self._plot_t2l(rows, png)
        except Exception as e:
            print(f"[警告] 绘图失败：{e}")
        print(f"[完成] 输出文件：\n  {xlsx}\n  {png}")

    def _write_xlsx(self, rows: list[list[str]], path: str):
        from openpyxl import Workbook
        wb = Workbook()
        ws = wb.active
        ws.title = "历史数据"
        ws.append(EXPORT_COLS)
        for row in rows:
            ws.append([self._num(v) for v in row])
        # 列宽微调
        for i, name in enumerate(EXPORT_COLS, 1):
            ws.column_dimensions[ws.cell(1, i).column_letter].width = max(10, len(name) * 2)
        wb.save(path)
        print(f"[OK] Excel 已保存：{path}")

    @staticmethod
    def _num(v: str):
        v = v.strip().rstrip("%")
        try:
            f = float(v)
            return int(f) if f.is_integer() and "." not in v else round(f, 4)
        except ValueError:
            return v

    def _plot_t2l(self, rows: list[list[str]], path: str):
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
        plt.rcParams["axes.unicode_minus"] = False

        # 按 L 分组求 Tavg 均值 → T²
        by_l: dict[float, list[float]] = {}
        for row in rows:
            try:
                L = float(row[2]); T = float(row[4])
            except (IndexError, ValueError):
                continue
            by_l.setdefault(L, []).append(T)
        if len(by_l) < 2:
            print("[提示] 不同摆长组数 <2，T²–L 图需至少两组不同 L 的数据；已保存数据点标记图。")

        Ls = sorted(by_l)
        T2 = [(sum(by_l[l]) / len(by_l[l])) ** 2 for l in Ls]   # 每组 Tavg 均值再平方

        fig, ax = plt.subplots(figsize=(7, 5), dpi=120)
        ax.plot(Ls, T2, "o-", color="#c0392b", label="实测 T²–L")
        ax.set_xlabel("摆长 L (m)")
        ax.set_ylabel("周期平方 T² (s²)")
        title = "单摆实验：T² – L 关系"

        g_est = None
        if len(Ls) >= 2:
            # 最小二乘斜率 k = 4π²/g
            n = len(Ls)
            sx, sy = sum(Ls), sum(T2)
            sxx = sum(l * l for l in Ls)
            sxy = sum(l * t for l, t in zip(Ls, T2))
            denom = n * sxx - sx * sx
            if denom != 0:
                k = (n * sxy - sx * sy) / denom
                if k > 0:
                    g_est = 4 * math.pi ** 2 / k
                    xs = [min(Ls), max(Ls)]
                    ax.plot(xs, [k * x for x in xs], "--", color="#2c3e50",
                            label=f"拟合斜率 k={k:.3f}")
        if g_est:
            title += f"    g = 4π²/k ≈ {g_est:.3f} m/s²"
        ax.set_title(title)
        ax.grid(alpha=0.3)
        ax.legend()
        fig.savefig(path)
        plt.close(fig)
        print(f"[OK] T²–L 图已保存：{path}"
              + (f"（拟合 g≈{g_est:.3f} m/s²）" if g_est else "（数据不足，未拟合 g）"))


async def main():
    address = sys.argv[1] if len(sys.argv) > 1 else None
    cli = BleCli(address)
    await cli.connect()

    loop = asyncio.get_running_loop()
    stop = asyncio.Event()

    async def input_loop():
        while True:
            msg = await loop.run_in_executor(None, input)
            msg = msg.strip()
            if msg.lower() == "q":
                if cli.exporting:
                    print("导出进行中，请等待 #EXPORT END（或 20s 无数据自动降级落盘）后再退出。")
                    continue
                stop.set()
                return
            if not msg:
                continue
            if cli.exporting:
                # 导出期间任意输入都会被固件当成菜单命令处理，可能把导出流程打断
                print("导出进行中：仅接受 q（且需等导出结束）；该输入已忽略。")
                continue
            await cli.send(msg)

    async def idle_watch():
        await stop.wait()
        if cli.client and cli.client.is_connected:
            await cli.client.disconnect()

    await asyncio.gather(input_loop(), idle_watch())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n已退出。")
