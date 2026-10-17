#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
phyphox_stream.py —— 单摆测g 实时波形客户端（phyphox 兼容）

功能：
  - 通过 BLE NUS 连接开发板，接收 #STREAM 实时数据流
  - matplotlib 动态绘图：周期 T / 即时 g / 平均 g 三条实时曲线
  - 运行统计面板（N、Tavg、Gavg、数据率）
  - 一键导出 CSV（含全部已收帧）
  - 自动重连 + 缓冲区防丢

用法：
  python phyphox_stream.py                  # 自动扫描 pendulum-g 并连接
  python phyphox_stream.py AA:BB:CC:DD:EE:FF  # 指定地址直连

依赖：bleak / matplotlib（见 requirements.txt）
协议：NUS #STREAM BEGIN → #STREAM <seq>,<dt_us>,<T>,<g_inst>,<g_avg>,<n>,<L> → #STREAM END

按键（需点一下绘图窗口使其获得焦点）：
  s   开始/停止推送（STREAM ON/OFF）
  e   导出当前缓冲为 CSV
  q   退出

线程结构：matplotlib/Tk 必须在主线程，因此本工具改为 asyncio（BLE）跑工作线程、
绘图跑主线程。无可用交互后端时自动降级为无窗口模式（每 3 秒刷新 PNG 快照 +
独立键盘线程）。
"""

from __future__ import annotations

import asyncio
import csv
import math
import os
import sys
import threading
import time
from datetime import datetime

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("缺少 bleak 库：请先执行  pip install -r requirements.txt")

# ---------- NUS UUID（与固件 fw_ble.cpp 一致） ----------
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

DEVICE_NAME = "pendulum-g"

# ---------- 流数据帧 ----------
# 固件格式：#STREAM <seq>,<dt_us>,<T>,<g_inst>,<g_avg>,<n>,<L>
class StreamFrame:
    __slots__ = ("seq", "dt_us", "T", "g_inst", "g_avg", "n", "L", "ts")

    def __init__(self, seq, dt_us, T, g_inst, g_avg, n, L):
        self.seq = int(seq)
        self.dt_us = int(dt_us)
        self.T = float(T)
        self.g_inst = float(g_inst)
        self.g_avg = float(g_avg)
        self.n = int(n)
        self.L = float(L)
        self.ts = time.time()

    @staticmethod
    def parse(line):
        """解析 '#STREAM <csv>' 行，返回 StreamFrame 或 None"""
        parts = line.split(",")
        if len(parts) >= 7:
            try:
                return StreamFrame(*parts[:7])
            except (ValueError, TypeError):
                return None
        return None


class PhyphoxStreamClient:
    def __init__(self, address=None):
        self.address = address
        self.client = None
        self.streaming = False          # 是否已发 STREAM ON
        self.active = False             # 是否收到过 STREAM BEGIN
        self.frames: list[StreamFrame] = []   # 环形缓冲（最近 500 帧）
        self.max_frames = 500
        self.total_rx = 0               # 总接收帧数
        self.begin_ts = 0.0             # STREAM BEGIN 时间戳
        self._last_key_time = 0.0       # 防按键抖动
        # 退出协调：断连回调运行在 bleak 的回调线程里，绝不能在此线程 sys.exit
        # （非主线程抛 SystemExit 只会杀死该线程，主程序会一直卡在阻塞处）
        self.stop = threading.Event()
        self.ready = threading.Event()
        self._last_seq = None           # 序号连续性校验基准（BLE notify 无逐包 ACK，丢帧靠 seq 跳变发现）

    # ==================== BLE 连接 ====================

    async def connect(self):
        if self.address:
            dev = await BleakScanner.find_device_by_address(self.address, timeout=10)
        else:
            print(f"正在扫描 {DEVICE_NAME} ...")
            devs = await BleakScanner.discover(timeout=5)
            dev = next((d for d in devs if (d.name or "").lower() == DEVICE_NAME), None)
            if dev is None:
                named = [d for d in devs if d.name]
                print("未找到开发板。附近可见 BLE 设备：")
                for d in named[:15]:
                    print(f"  {d.address}  {d.name}")
                sys.exit("请确认开发板已上电、未被手机 App 占用，或用地址直连。")
        print(f"连接 {dev.address} ({dev.name}) ...")
        self.client = BleakClient(dev, disconnected_callback=self._on_disconnect)
        await self.client.connect()
        await self.client.start_notify(NUS_TX, self._on_notify)
        print("已连接。")
        print("=" * 60)
        print("  单摆测g · 实时波形（phyphox 兼容）")
        print("-" * 60)
        print("  s   开始/停止实时推送 (STREAM ON/OFF)")
        print("  e   导出当前缓冲为 CSV")
        print("  q   退出")
        print("=" * 60)
        print("\n提示：请先在开发板上切换到 [2] 单摆测g 程序，")
        print("      再按 's' 开始推送；开发板需处于测量中状态。\n")

    def _on_disconnect(self, _):
        # 只置标志，由主线程/主循环负责收尾；在此处 sys.exit 会把工具卡死
        print("\n[连接已断开] 窗口将在 1 秒内关闭；如需继续请重启本工具。")
        self.active = False
        self.streaming = False
        self.stop.set()

    def _on_notify(self, _ch, data: bytearray):
        text = data.decode("utf-8", errors="replace")
        for line in text.splitlines():
            self._handle_line(line.strip())

    def _handle_line(self, line: str):
        if not line:
            return
        if line == "#STREAM BEGIN":
            self.active = True
            self.begin_ts = time.time()
            self._last_seq = None       # 新流重置序号基准
            print("[流开始] 等待数据...")
            return
        if line == "#STREAM END":
            was_active = self.active
            self.active = False
            self.streaming = False
            if was_active:
                dur = time.time() - self.begin_ts if self.begin_ts > 0 else 0
                print(f"[流结束] 共收 {self.total_rx} 帧，持续 {dur:.1f}s")
            return
        if line.startswith("#STREAM "):
            frame = StreamFrame.parse(line[len("#STREAM "):])
            if frame:
                # 序号连续性校验：BLE notify 无逐包 ACK，丢帧表现为 seq 跳变；
                # 明确警告而非静默吞掉（对齐第三轮 R3-02 的严格校验纪律）
                if self._last_seq is not None and frame.seq != self._last_seq + 1:
                    print(f"  [警告] 序号跳变 {self._last_seq} → {frame.seq}"
                          f"（丢失 {frame.seq - self._last_seq - 1} 帧）")
                self._last_seq = frame.seq
                self.frames.append(frame)
                if len(self.frames) > self.max_frames:
                    self.frames.pop(0)
                self.total_rx += 1
                # 每 10 帧打印一次简报（不刷屏）
                if self.total_rx % 10 == 1 and len(self.frames) >= 1:
                    recent = self.frames[-1]
                    print(f"  #{recent.seq:04d}  T={recent.T:.4f}s  "
                          f"g={recent.g_inst:.2f}  Gavg={recent.g_avg:.2f}  "
                          f"n={recent.n}  L={recent.L:.3f}m")
            return
        # 板端拒绝（如程序未切到 [2]）：回退本地开关状态，避免客户端与板端状态漂移
        # （toggle_stream 乐观置位，板端 ERR 即本次切换未生效，下次按 s 会重发 ON）
        if line.startswith("ERR:"):
            self.streaming = False
        # 其他行（OK:/ERR:/INFO:）直接显示
        if line:
            print(f"  {line}")

    async def send(self, msg: str):
        if self.client is None or not self.client.is_connected:
            print("[错误] 未连接")
            return
        await self.client.write_gatt_char(NUS_RX, (msg + "\n").encode("utf-8"))

    async def toggle_stream(self):
        """切换 STREAM ON/OFF"""
        now = time.time()
        if now - self._last_key_time < 0.5:
            return
        self._last_key_time = now

        if not self.streaming:
            await self.send("STREAM ON")
            self.streaming = True
            print(">> 已发送 STREAM ON（等待 #STREAM BEGIN）...")
        else:
            await self.send("STREAM OFF")
            self.streaming = False
            print(">> 已发送 STREAM OFF")

    async def request_stop(self):
        """请求停止：断开 BLE 并置位退出标志（供其他线程调用）"""
        self.stop.set()
        if self.client is not None and self.client.is_connected:
            try:
                await self.client.disconnect()
            except Exception as e:
                print(f"[提示] 断开时出错：{e}")

    async def wait_stop_async(self):
        """协程版等待退出信号"""
        while not self.stop.is_set():
            await asyncio.sleep(0.2)

    def export_csv(self, path=None):
        """导出缓冲帧为 CSV"""
        if not self.frames:
            print("[提示] 无数据可导出")
            return
        if path is None:
            stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            path = f"pendulum_stream_{stamp}.csv"
        cols = ["序号", "半周期(us)", "周期T(s)", "即时g", "平均g", "半周期数n", "摆长L(m)", "接收时间"]
        with open(path, "w", newline="", encoding="utf-8-sig") as f:
            w = csv.writer(f)
            w.writerow(cols)
            for fr in self.frames:
                t_str = datetime.fromtimestamp(fr.ts).strftime("%H:%M:%S.") + f"{int((fr.ts % 1) * 1000):03d}"
                w.writerow([fr.seq, fr.dt_us, f"{fr.T:.6f}", f"{fr.g_inst:.4f}",
                            f"{fr.g_avg:.4f}", fr.n, f"{fr.L:.3f}", t_str])
        print(f"[OK] 已导出 {len(self.frames)} 帧 -> {path}")


# ==================== matplotlib 动态绘图 ====================

SNAPSHOT_PNG = "pendulum_stream_live.png"


def pick_backend():
    """挑一个可用的交互式后端；没有则退回 Agg（无窗口模式）。

    TkAgg 必须在主线程调用 plt.show()——这是整个线程结构调整的唯一原因：
    原写法把 show() 放在 daemon 子线程里，多数平台会直接抛
    "RuntimeError: main thread is not in main loop" 或窗口无响应。
    """
    import matplotlib

    for name in ("TkAgg", "QtAgg", "Qt5Agg", "GTK3Agg", "WXAgg"):
        try:
            matplotlib.use(name, force=True)
            import matplotlib.pyplot as plt
            fig = plt.figure()
            plt.close(fig)
            return name
        except Exception:
            continue
    matplotlib.use("Agg", force=True)
    return None


def run_plotter(client: "PhyphoxStreamClient", loop, interactive: bool):
    """在主线程构建并驱动 matplotlib（TkAgg 要求；其余后端同样受益）"""
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation

    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False

    fig, axes = plt.subplots(3, 1, figsize=(9, 7), sharex=True)
    fig.suptitle("单摆测g · 实时波形（等待数据...）", fontsize=12)

    ax_t, ax_g, ax_gavg = axes
    ax_t.set_ylabel("周期 T (s)")
    ax_t.grid(alpha=0.3)
    ax_g.set_ylabel("即时 g (m/s²)")
    ax_g.grid(alpha=0.3)
    ax_gavg.set_ylabel("平均 g (m/s²)")
    ax_gavg.grid(alpha=0.3)
    ax_gavg.set_xlabel("帧序号")

    lines_t,     = ax_t.plot([], [], "b-", lw=0.8, label="T")
    lines_g,     = ax_g.plot([], [], "r-", lw=0.8, label="g_inst")
    lines_gavg,  = ax_gavg.plot([], "g-", lw=0.8, label="Gavg")
    ax_t.legend(loc="upper right"); ax_g.legend(loc="upper right"); ax_gavg.legend(loc="upper right")

    # 统计文本（注意：不要强制 family="monospace"——多数系统的等宽字体不含
    # CJK 字形，会把中文统计面板渲染成豆腐块；改用与标题一致的 sans-serif 回退链，
    # 由 rcParams 的 Microsoft YaHei/SimHei 兜底，中文正常显示）
    stat_text = fig.text(0.02, 0.01, "", fontsize=9,
                         verticalalignment="bottom")

    max_display = 200  # 屏幕最多显示最近 200 帧

    def update(_frame_num):
        frames = client.frames
        n = len(frames)
        if n == 0:
            return lines_t, lines_g, lines_gavg, stat_text

        start = max(0, n - max_display)
        sub = frames[start:]
        xs = [f.seq for f in sub]
        ts = [f.T for f in sub]
        gs = [f.g_inst for f in sub]
        gas = [f.g_avg for f in sub]

        lines_t.set_data(xs, ts)
        lines_g.set_data(xs, gs)
        lines_gavg.set_data(xs, gas)

        # 自动缩放
        for ax, ys in [(ax_t, ts), (ax_g, gs), (ax_gavg, gas)]:
            if xs:
                ax.set_xlim(xs[0], xs[-1])
                y_min, y_max = min(ys), max(ys)
                margin = (y_max - y_min) * 0.1 + 1e-6
                ax.set_ylim(y_min - margin, y_max + margin)

        # 标题更新
        last = frames[-1]
        status = "测量中" if client.active else "待机"
        fig.suptitle(f"单摆测g · 实时波形 [{status}]  "
                     f"L={last.L:.3f}m  N={last.n}",
                     fontsize=11)

        # 统计面板
        if n > 1:
            T_arr = [f.T for f in frames]
            g_arr = [f.g_avg for f in frames]
            avg_T = sum(T_arr) / len(T_arr)
            avg_g = sum(g_arr) / len(g_arr)
            dur = time.time() - client.begin_ts if client.begin_ts > 0 else 0
            rate = client.total_rx / dur if dur > 0 else 0
            stat_text.set_text(
                f"总帧数: {client.total_rx}  |  缓冲: {n}/{client.max_frames}  |  "
                f"Tavg={avg_T:.4f}s  Gavg={avg_g:.3f}m/s²  |  "
                f"数据率: {rate:.1f} fps  |  持续: {dur:.1f}s")
        else:
            stat_text.set_text(f"总帧数: {client.total_rx}  |  等待更多数据...")

        return lines_t, lines_g, lines_gavg, stat_text

    def schedule(coro):
        """把协程丢给 asyncio 工作线程执行（matplotlib 回调在主线程，不能直接 await）"""
        if loop is None:
            return
        try:
            asyncio.run_coroutine_threadsafe(coro, loop)
        except RuntimeError as e:
            print(f"[提示] 命令未送达：{e}")

    def on_key(event):
        k = (event.key or "").lower()
        if k == "s":
            schedule(client.toggle_stream())
        elif k == "e":
            client.export_csv()
        elif k == "q":
            print(">> 正在退出...")
            schedule(client.request_stop())
            try:
                plt.close("all")
            except Exception:
                pass

    ani = FuncAnimation(fig, update, interval=200, blit=False, cache_frame_data=False)  # noqa: F841

    if interactive:
        # 键盘由绘图窗口捕获，而不是再开一个 input() 阻塞主线程——
        # 主线程必须留给 Tk 的事件循环，否则窗口会卡死无响应
        fig.canvas.mpl_connect("key_press_event", on_key)
        print("提示：按键请点一下绘图窗口使其获得焦点（s/e/q）。")
        plt.show()
    else:
        # 无窗口模式：键盘走独立线程，画面定时刷成快照 PNG
        kbd = threading.Thread(target=_keyboard_loop, args=(client, loop), daemon=True)
        kbd.start()
        print(f"提示：无可用交互后端，改为每 3 秒刷新快照 {SNAPSHOT_PNG}")
        while not client.stop.is_set():
            try:
                update(0)
                fig.savefig(SNAPSHOT_PNG, dpi=100)
            except Exception as e:
                print(f"[提示] 快照刷新失败：{e}")
            client.stop.wait(3)
        plt.close(fig)


def _keyboard_loop(client: "PhyphoxStreamClient", loop):
    """无窗口模式下的键盘输入线程"""
    while not client.stop.is_set():
        try:
            msg = input().strip().lower()
        except (EOFError, OSError):
            return
        if msg == "q":
            if loop is not None:
                try:
                    asyncio.run_coroutine_threadsafe(client.request_stop(), loop)
                except RuntimeError:
                    pass
            else:
                client.stop.set()
            return
        if msg == "s":
            asyncio.run_coroutine_threadsafe(client.toggle_stream(), loop)
        elif msg == "e":
            client.export_csv()


# ==================== 主循环 ====================

def main():
    address = sys.argv[1] if len(sys.argv) > 1 else None
    client = PhyphoxStreamClient(address)

    backend = pick_backend()
    interactive = backend is not None
    if interactive:
        print(f"[绘图后端] {backend}")
    else:
        print("[绘图后端] 未找到可用的交互式后端（Tk/Qt/GTK/WX 均不可用），"
              f"已切换为无窗口快照模式（每 3 秒刷新 {SNAPSHOT_PNG}）。")

    loop = asyncio.new_event_loop()

    async def runner():
        asyncio.set_event_loop(loop)
        try:
            await client.connect()
        except Exception as e:
            print(f"[错误] 连接失败：{e}")
            client.stop.set()
            return
        finally:
            client.ready.set()          # 无论成败都要放行主线程
        try:
            await client.wait_stop_async()
        finally:
            if client.client is not None and client.client.is_connected:
                try:
                    await client.client.disconnect()
                except Exception:
                    pass

    def worker():
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(runner())
        finally:
            try:
                loop.run_until_complete(loop.shutdown_asyncgens())
            except Exception:
                pass
            loop.close()

    th = threading.Thread(target=worker, daemon=True)
    th.start()

    if not client.ready.wait(timeout=30):
        print("[错误] 30 秒内未完成连接。")
        client.stop.set()
        return 1
    if client.stop.is_set():            # 连接阶段就失败
        return 1

    # 主线程专门跑 matplotlib（TkAgg 硬要求）
    try:
        run_plotter(client, loop, interactive)
    finally:
        client.stop.set()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已退出。")
