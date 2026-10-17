#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_cmd_verify.py —— 串口下发命令验证新固件（绕开 Windows BLE 不可用）
前提：fw_ble 已实现串口命令通道，读到行后复用 handleBleMsg，回复自动落串口。
用法：python serial_cmd_verify.py
"""
import sys
import time
import serial

PORT = "COM22"


def drain(ser, tag=""):
    """把缓冲里已有的输出一并收下（用于消掉复位横幅）"""
    out = []
    t0 = time.time()
    while time.time() - t0 < 1.5:
        line = ser.readline()
        if line:
            out.append(line.decode("utf-8", "replace").rstrip())
        else:
            time.sleep(0.05)
    if out and tag:
        print(f"--- {tag} ---")
        for l in out:
            print("   ", l)
    return out


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.15)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.flushInput()
    ser.reset_input_buffer()

    print(f"打开 {PORT}，等待固件就绪…")
    boot = drain(ser, "启动日志")
    if not any("TaskManagerFirmware" in l for l in boot):
        print("警告：未捕获到启动横幅（可能已启动完毕，忽略）")

    def send(cmd, wait=6.0, tag=""):
        print(f"\n>>> 发送: {cmd}")
        ser.write((cmd + "\n").encode("utf-8"))
        ser.flush()
        time.sleep(0.3)
        lines = drain(ser, tag or cmd)
        if not lines:
            print("    （无回复）")
        return lines

    def menu_then(sub, tag=""):
        """天气/摆长菜单是一次性的（执行子项即 endSession），
        故每条子命令都必须重新先进菜单。"""
        send(sub[0], tag="(进菜单)")
        send(sub[1], tag=tag or sub[1])

    # 1) 系统状态总览：确认三程序与天气凭据状态
    send("0", tag="[0] 状态总览")
    # 2) 天气菜单：查看 Key 配置状态
    menu_then(("4", "4.1"), tag="[4.1] 天气状态")
    # 3) 切到天气程序并立即更新
    send("APP 3", tag="切到程序3")
    menu_then(("4", "4.3"), tag="[4.3] 立即更新")
    # 4) 等拉取（QWeather 两次请求 + 阻塞 5-15s）
    print("\n=== 等待天气拉取（35s）===")
    drain(ser, "天气拉取日志")
    # 5) 回读确认
    menu_then(("4", "4.1"), tag="[4.1] 更新后状态")
    # 6) 切回单摆，检查配对字段
    send("APP 2", tag="切回程序2")
    menu_then(("2", "altstat"), tag="[altstat] 交替诊断")
    menu_then(("2", "2.1"), tag="[2.1] 摆长与配对")
    # 7) 无光电门自测：SIM 注入已知半周期，验证配对结算的数值正确性
    send("SIM 0.5", tag="SIM 半周期 0.5s（理论 T=1s, g=9.8696）")
    print("\n=== 等待 SIM 累积（12s）===")
    time.sleep(12)
    drain(ser, "SIM 运行日志")
    menu_then(("2", "altstat"), tag="[altstat] SIM 后")

    ser.close()
    print("\n=== 完成 ===")


if __name__ == "__main__":
    main()
