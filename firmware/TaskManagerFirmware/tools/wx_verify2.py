#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wx_verify2.py —— 天气全链路验证（正确 Host 版）
流程：切程序3 → 清除 NVS 旧凭据改用内置 → 立即更新 → 观察拉取 → 回读状态
"""
import time
import serial

PORT = "COM22"


def main():
    ser = serial.Serial(PORT, 115200, timeout=0.15)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.flushInput()
    ser.reset_input_buffer()

    def drain(secs, tag=""):
        """用 read() 按缓冲区取，不用 readline()。
        原因：ESP32 阻塞式拉取天气时会有十几秒不输出，pyserial 的
        readline() 超时会抛 ClearCommError（Windows 批量传输超时）导致脚本中断。
        read() + in_waiting 更耐受长时间静默。"""
        out = []
        buf = b""
        t0 = time.time()
        while time.time() - t0 < secs:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    s = line.decode("utf-8", "replace").rstrip()
                    out.append(s)
                    print("   ", s)
            else:
                time.sleep(0.05)
        return out

    def send(cmd):
        print(f"\n>>> {cmd}")
        ser.write((cmd + "\n").encode("utf-8"))
        ser.flush()
        time.sleep(0.3)

    print("=== 复位后等待启动 ===")
    drain(6)

    print("\n=== 1) 切到程序3（天气）===")
    send("APP 3")
    drain(3)

    print("\n=== 2) 清除 NVS 旧凭据，改用固件内置（关键：NVS 优先于内置）===")
    send("4")
    drain(2)
    send("4.5")
    drain(3)

    print("\n=== 3) 等待拉取（WiFi 连接 + now/24h 两次请求，阻塞 5-15s）===")
    drain(40)

    print("\n=== 4) 回读状态 ===")
    send("4")
    drain(2)
    send("4.1")
    drain(3)

    ser.close()


if __name__ == "__main__":
    main()
