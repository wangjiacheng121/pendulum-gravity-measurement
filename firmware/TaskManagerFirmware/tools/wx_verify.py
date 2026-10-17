#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wx_verify.py —— 专门验证程序3 天气：内置凭据是否真能拉到数据
"""
import time
import serial

PORT = "COM22"


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.4)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.flushInput()
    ser.reset_input_buffer()

    def drain(secs, tag=""):
        out = []
        t0 = time.time()
        while time.time() - t0 < secs:
            line = ser.readline()
            if line:
                s = line.decode("utf-8", "replace").rstrip()
                out.append(s)
                print("   ", s)
        return out

    def send(cmd):
        print(f"\n>>> {cmd}")
        ser.write((cmd + "\n").encode("utf-8"))
        ser.flush()
        time.sleep(0.3)

    print("=== 复位后等待启动 ===")
    drain(6, "boot")

    print("\n=== 切到程序3（天气）===")
    send("APP 3")
    drain(3, "activate")

    print("\n=== 触发立即更新 ===")
    send("4")      # 进菜单
    drain(2, "menu")
    send("4.3")    # 立即更新
    print("    等待 40s 拉取（含 WiFi 连接 + 两次 HTTP 请求）...")
    drain(40, "fetch")

    print("\n=== 读取状态 ===")
    send("4")
    drain(2, "menu2")
    send("4.1")
    drain(3, "status")

    ser.close()


if __name__ == "__main__":
    main()
