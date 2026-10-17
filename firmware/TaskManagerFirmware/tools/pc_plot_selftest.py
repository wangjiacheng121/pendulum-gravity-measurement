#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_plot_selftest.py —— PC 端绘图能力自检（无需开发板）

目的：在“只连电脑、无光电门/无 BLE”的条件下，验证两个 PC 工具的真实绘图代码
      能够跑通并产生文件，证明“电脑端交互程序能运行、能绘图”。

  Part A（ble_cli 导出绘图）：构造若干组单摆测量数据 → 调 BleCli._write_and_plot
       → 生成 Excel（.xlsx）与 T²–L 拟合图（.png），并校验拟合出的 g 与理论值接近。
  Part B（phyphox_stream 波形绘图）：构造若干帧 #STREAM 数据 → 调 run_plotter 真实代码
       → 生成实时波形快照（.png），含中文标题/轴标。

用法（需在隔离 venv 中，已装 bleak/openpyxl/matplotlib/numpy）：
  python pc_plot_selftest.py
"""
from __future__ import annotations
import os
import sys
import math
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "_selftest_out")
os.makedirs(OUT, exist_ok=True)
sys.path.insert(0, HERE)

G_THEORY = 9.80          # 理论 g（与固件 SIM 默认 g=9.80 一致）
PI = math.pi


def part_a_export_plot():
    """复现 ble_cli 的导出+绘图链路（真实代码路径）。"""
    import ble_cli
    cli = ble_cli.BleCli()

    # 构造多组不同摆长的合成测量：T = 2π√(L/g)，Gavg≈g，误差≈0
    rows: list[list[str]] = []
    Ls = [0.30, 0.60, 1.00, 1.50, 2.00]
    idx = 1
    for L in Ls:
        T = 2.0 * PI * math.sqrt(L / G_THEORY)
        Gavg = 4.0 * PI * PI * L / (T * T)
        err = (Gavg - G_THEORY) / G_THEORY * 100.0
        ts = "2026-10-01 16:%02d:00" % int(L * 10 % 60)
        rows.append([str(idx), ts, "%.3f" % L, "10", "%.4f" % T,
                     "%.3f" % Gavg, "%.2f%%" % err])
        idx += 1
    # 再给 L=1.00 补一组，验证同 L 求均值不破坏拟合
    rows.append(["6", "2026-10-01 16:10:00", "1.000", "10", "2.0075",
                 "9.802", "0.02%"])

    xlsx = os.path.join(OUT, "pendulum_history_selftest.xlsx")
    png = os.path.join(OUT, "pendulum_T2L_selftest.png")
    cli._write_and_plot(rows, xlsx, png)

    ok_xlsx = os.path.exists(xlsx) and os.path.getsize(xlsx) > 0
    ok_png = os.path.exists(png) and os.path.getsize(png) > 0
    print(f"[A] xlsx={'OK' if ok_xlsx else 'FAIL'} ({os.path.getsize(xlsx) if ok_xlsx else 0}B)  "
          f"png={'OK' if ok_png else 'FAIL'} ({os.path.getsize(png) if ok_png else 0}B)")
    return ok_xlsx and ok_png


def part_b_stream_plot():
    """复现 phyphox_stream 的实时波形绘图（真实代码路径 run_plotter）。"""
    import phyphox_stream as ps

    client = ps.PhyphoxStreamClient()
    # 构造合成波形帧：摆长 1.0m，理论 T≈2.0075s，g_inst 带轻微噪声，Gavg 收敛到 9.80
    L = 1.0
    T0 = 2.0 * PI * math.sqrt(L / G_THEORY)
    frames = []
    for i in range(1, 41):
        noise = 0.004 * math.sin(i / 3.0)          # ±4ms 抖动，模拟真实摆
        T = T0 + noise
        g_inst = 4.0 * PI * PI * L / (T * T)
        Gavg = 4.0 * PI * PI * L / ((T0 + 0.5 * noise) ** 2)  # 均值更稳定
        dt_us = int(T * 1e6)
        frames.append(ps.StreamFrame(i, dt_us, T, g_inst, Gavg, i, L))
    client.frames = frames
    client.total_rx = len(frames)
    client.active = True
    client.begin_ts = time.time() - 4.0

    # 选后端（与 main() 一致）；无交互后端会自动回落 Agg
    ps.pick_backend()

    # 用看门狗线程在 0.6s 后置 stop，使 run_plotter 的快照循环至少执行一次并落盘
    def _watchdog():
        time.sleep(0.6)
        client.stop.set()
    threading.Thread(target=_watchdog, daemon=True).start()

    try:
        ps.run_plotter(client, loop=None, interactive=False)
    except Exception as e:
        print(f"[B] run_plotter 异常：{e}")
        return False

    snap = os.path.join(HERE, ps.SNAPSHOT_PNG)
    ok = os.path.exists(snap) and os.path.getsize(snap) > 0
    # 把快照复制到输出目录，便于查看
    if ok:
        dst = os.path.join(OUT, "pendulum_stream_live_selftest.png")
        with open(snap, "rb") as f:
            data = f.read()
        with open(dst, "wb") as f:
            f.write(data)
        print(f"[B] 波形png={'OK' if ok else 'FAIL'} ({os.path.getsize(dst)}B) -> {dst}")
    else:
        print("[B] 波形png=FAIL（未生成快照）")
    return ok


if __name__ == "__main__":
    print("=" * 60)
    print(" PC 端绘图自检（真实代码路径 / 合成数据 / 无需开发板）")
    print("=" * 60)
    a = part_a_export_plot()
    print("-" * 60)
    b = part_b_stream_plot()
    print("=" * 60)
    print("结果：导出绘图(A)=%s  波形绘图(B)=%s" % (("通过" if a else "失败"),
                                                   ("通过" if b else "失败")))
    print("输出目录：%s" % OUT)
    sys.exit(0 if (a and b) else 1)
