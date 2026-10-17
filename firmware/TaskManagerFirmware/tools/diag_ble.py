#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
diag_ble.py —— 综合诊断：
  1) 通过 COM23(CH340) 脉冲 DTR 复位板子，抓完整开机日志确认固件 Serial 落在哪个口；
  2) 监听 COM22(原生 CDC) 同时做多次 BLE 连接尝试，记录板子视角的 BLE 事件。
日志写 diag_ble.log，控制台也打印。
"""
import sys, time, datetime, threading
import serial

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("缺少 bleak")

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
DEVICE_NAME = "pendulum-g"
KNOWN = "3C:0F:02:DD:2B:59"
LOG = "diag_ble.log"

stop = threading.Event()


def main():
    f = open(LOG, "w", encoding="utf-8")
    def log(ts, tag, line):
        out = f"[{ts}] {tag} {line}"
        print(out); f.write(out + "\n"); f.flush()

    # 1) 复位板子（COM23 DTR 脉冲）
    try:
        s = serial.Serial("COM23", 115200)
        s.dtr = False; time.sleep(0.05); s.dtr = True; time.sleep(0.05); s.dtr = False
        s.close()
        print("[*] 已向 COM23 发送复位脉冲，等待板子启动…")
    except Exception as e:
        print(f"[!] COM23 复位脉冲失败（忽略）：{e}")
    time.sleep(2.5)

    # 2) 开 COM22 监听（原生 CDC，被动不复位）
    try:
        ser = serial.Serial("COM22", 115200, timeout=0.3)
        ser.dtr = False; ser.rts = False; ser.flushInput()
    except Exception as e:
        print(f"[!] 无法打开 COM22：{e}"); ser = None

    def reader():
        if ser is None: return
        while not stop.is_set():
            try:
                raw = ser.read(200)
            except Exception as e:
                log(datetime.datetime.now().strftime("%H:%M:%S"), "SER-ERR", str(e)); break
            if not raw: continue
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            for ln in raw.decode("utf-8", errors="replace").splitlines():
                if ln.strip():
                    log(ts, "SER", ln)

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    # 3) BLE 多次尝试（每次重新发现设备，避免服务缓存失效）
    async def ble():
        for i in range(1, 4):
            print(f"\n--- BLE 尝试 {i} ---")
            dev = await BleakScanner.find_device_by_address(KNOWN, timeout=10)
            if dev is None:
                devs = await BleakScanner.discover(timeout=6)
                dev = next((d for d in devs if (d.name or "").lower() == DEVICE_NAME), None)
            if dev is None:
                print("  [BLE] 找不到设备，跳过"); await asyncio.sleep(1); continue
            print(f"  目标 {dev.address} ({dev.name})")
            c = BleakClient(dev)
            try:
                await c.connect(timeout=15)
                await c.start_notify(NUS_TX, lambda *_: None)
                print(f"  [BLE] connect+notify OK")
                await c.disconnect()
            except Exception as e:
                print(f"  [BLE] 失败: {e}")
            await asyncio.sleep(2)

    try:
        asyncio.run(ble())
    except Exception as e:
        print(f"[!] BLE 异常：{e}")

    time.sleep(1)
    stop.set()
    if ser: ser.close()
    f.close()
    print(f"\n[*] 诊断结束，日志 {LOG}。请查看 SER 行里是否有 [BLE] connected/disconnected、rst:、Brownout 等。")


if __name__ == "__main__":
    import asyncio
    main()
