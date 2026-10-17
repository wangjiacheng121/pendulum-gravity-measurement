# -*- coding: utf-8 -*-
"""multil_with_serial.py —— 多摆长测试 + 串口观测"""
import asyncio, sys, io, os, time, threading
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import serial

def now(): return time.strftime("%H:%M:%S") + f".{int(time.time()*1000)%1000:03d}"

serial_lines = []
stop = threading.Event()
def serial_reader():
    try:
        s = serial.Serial("COM22", 115200, timeout=0.5)
    except Exception as e:
        serial_lines.append(f"[SER] 打开失败: {e}"); return
    buf = b""
    while not stop.is_set():
        try:
            buf += s.read(512)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = line.decode("utf-8", "replace").strip()
                if t: serial_lines.append(f"[{now()}] {t}")
        except Exception:
            break
    s.close()

async def main():
    th = threading.Thread(target=serial_reader, daemon=True); th.start()
    await asyncio.sleep(1.0)
    import e2e_multil
    rc = await e2e_multil.main()
    stop.set(); await asyncio.sleep(1.0)
    print("=" * 60)
    print("=== 固件串口日志（完整时间线）===")
    for l in serial_lines:
        print(" ", l)
    return rc

sys.exit(asyncio.run(main()))
