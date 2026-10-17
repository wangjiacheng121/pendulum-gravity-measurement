#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_pend_n_test.py —— 串口验证「摆动时 n 不增加」问题（2026-10-02）

背景：用户反馈单摆程序里摆球通过光电门若干次，屏幕 n 一直不增。
怀疑两种成因：
  A) _measuring == false —— 没按开始测量，onTick 第 321 行 `if (!_measuring) continue;`
     会把 ISR 捕获的样本直接丢弃，n 恒为 0（这是设计行为，不是 bug）
  B) ISR 根本没收到边沿 —— 硬件/极性/接线问题

本脚本走串口（BLE 在本机不兼容），用 SIM 模式注入已知半周期：
  若 SIM 下 n 正常增长 → 排除 B，指向 A 或真实光电门信号问题
  若 SIM 下 n 仍不增 → 说明固件侧队列/消费链路有 bug

用法：python serial_pend_n_test.py COM22
"""
import sys
import time
import datetime
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM22"


def open_serial(port):
    ser = serial.Serial(port, 115200, timeout=0.3)
    ser.dtr = False
    ser.rts = False
    ser.flushInput()
    return ser


def drain(ser, secs):
    """读取并回显 secs 秒内的全部串口输出"""
    t0 = time.time()
    lines = []
    while time.time() - t0 < secs:
        raw = ser.read(200)
        if not raw:
            continue
        for ln in raw.decode("utf-8", errors="replace").splitlines():
            ln = ln.strip()
            if ln:
                lines.append(ln)
                print(f"    {ln}")
    return lines


def send(ser, cmd, wait=1.2):
    print(f"  >> {cmd}")
    ser.write((cmd + "\n").encode("utf-8"))
    ser.flush()
    time.sleep(0.15)
    return drain(ser, wait)


def main():
    print(f"=== 串口单摆 n 自增验证 {PORT} @115200 ===")
    print(f"时间: {datetime.datetime.now().isoformat()}\n")
    ser = open_serial(PORT)

    try:
        print("[1] 复位板子")
        ser.setDTR(False)
        ser.reset_input_buffer()
        # 触发一次软复位：开 DTR 会让 ESP32-S3 复位（CH340 行为）
        ser.dtr = True
        time.sleep(0.12)
        ser.dtr = False
        time.sleep(2.5)
        drain(ser, 3.0)

        print("\n[2] 切到程序2（单摆测g）")
        send(ser, "APP 2", 1.5)

        print("\n[3] 开启 SIM 注入（L=0.425 → 半周期约 0.654s）")
        send(ser, "SIM ON", 1.5)

        print("\n[4] 短按开始测量（关键！不按则 n 恒为 0，属设计行为）")
        send(ser, "2", 1.5)          # 进 BLE 菜单非按键路径，这里改用 APP 后由用户按键
        # 串口没有 onButton 直通命令，用 SIM 自身逻辑：
        # SIM ON 在 app2 里会自动置 _measuring（见源码），此处仅观察

        print("\n[5] 观察 12 秒，看 n / 样本是否增长")
        lines = drain(ser, 12.0)
        joined = "\n".join(lines)
        markers = [k for k in ("n=", "N=", "SIM", "Pend", "结算", "丢样", "周期核对") if k in joined]
        print(f"\n  捕获到的关键行类型: {markers if markers else '（无）'}")

        print("\n[6] 查 [2.1] 统计")
        send(ser, "2.1", 2.0)
        send(ser, "2", 1.2)
        send(ser, "2.1", 2.0)

        print("\n[7] 关闭 SIM")
        send(ser, "SIM OFF", 1.0)

    finally:
        ser.close()
        print("\n=== 结束 ===")


if __name__ == "__main__":
    main()
