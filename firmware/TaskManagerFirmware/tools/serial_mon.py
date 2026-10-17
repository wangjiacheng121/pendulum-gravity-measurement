#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_mon.py —— 只读监听 ESP32-S3 CDC 串口，带时间戳，同时写日志文件。
用于抓 BLE 连接时的复位横幅（brownout / rst: 等），坐实连接秒断根因。

用法：python serial_mon.py COM22 40  _serial_dump.log
"""
import sys
import time
import datetime
import serial


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM22"
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    log = sys.argv[3] if len(sys.argv) > 3 else "_serial_dump.log"

    try:
        ser = serial.Serial(port, 115200, timeout=0.3)
        # 被动监听：关掉 DTR/RTS，避免 CH340 开串口时把 ESP32 复位
        ser.dtr = False
        ser.rts = False
        ser.flushInput()
    except Exception as e:
        sys.exit(f"无法打开 {port}: {e}")

    print(f"监听 {port} @115200，时长 {secs:.0f}s，日志->{log}")
    t0 = time.time()
    with open(log, "w", encoding="utf-8") as f:
        f.write(f"# serial_mon {port} start {datetime.datetime.now().isoformat()}\n")
        while time.time() - t0 < secs:
            try:
                raw = ser.read(200)
            except Exception as e:
                line = f"[读取出错] {e}"
                print(line); f.write(line + "\n"); break
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace")
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            for ln in text.splitlines():
                if ln.strip():
                    out = f"[{ts}] {ln}"
                    print(out)
                    f.write(out + "\n")
    ser.close()
    print(f"监听结束，日志已存 {log}")


if __name__ == "__main__":
    main()
