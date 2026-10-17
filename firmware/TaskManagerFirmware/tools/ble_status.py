#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ble_status.py —— 一次性读取开发板 BLE 状态总览（菜单 [0]）

用法：
  python ble_status.py                 # 自动扫描 pendulum-g
  python ble_status.py AA:BB:CC:DD:EE:FF   # 指定地址直连

连上后发送 "0" 并收集约 6 秒的 notify 文本后自动断开。
"""
import asyncio
import sys

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("缺少 bleak 库：请先执行  pip install -r requirements.txt")

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
DEVICE_NAME = "pendulum-g"
KNOWN_ADDRESS = "3C:0F:02:DD:2B:59"   # 上板测试时记录的 MAC（备用直连）

COLLECT_S = 5.0
RETRIES = 4


async def connect_once(dev, attempt):
    """连接一次、发 [0]、收 COLLECT_S 秒。返回收到的行列表（空=本次未拿到数据）。"""
    lines = []

    def on_notify(_ch, data: bytearray):
        text = data.decode("utf-8", errors="replace")
        for ln in text.splitlines():
            ln = ln.strip()
            if ln:
                lines.append(ln)
                print(f"  {ln}")

    print(f"\n[尝试 {attempt}] 连接 {dev.address} ({dev.name or '?'}) …")
    client = BleakClient(dev)
    try:
        await client.connect(timeout=15)
        await client.start_notify(NUS_TX, on_notify)
    except Exception as e:
        print(f"  [连接/订阅失败] {e}")
        try:
            await client.disconnect()
        except Exception:
            pass
        return None
    print("  已连接，发送状态命令 [0] …")
    try:
        await client.write_gatt_char(NUS_RX, b"0\n")
    except Exception as e:
        print(f"  [发送失败] {e}")
    try:
        await asyncio.sleep(COLLECT_S)
    except asyncio.CancelledError:
        pass
    was_connected = client.is_connected
    try:
        await client.disconnect()
    except Exception:
        pass
    print(f"  [尝试 {attempt}] 连接保持={was_connected}，收到 {len(lines)} 行。")
    return lines


async def run(address=None):
    # 先定位设备（扫描或直连），失败再回退已知地址
    dev = None
    if address:
        print(f"按地址直连 {address} …")
        dev = await BleakScanner.find_device_by_address(address, timeout=12)
    else:
        print(f"扫描 {DEVICE_NAME} …（约 8 秒）")
        try:
            devs = await BleakScanner.discover(timeout=8)
        except Exception as e:
            sys.exit(f"扫描失败（可能无蓝牙适配器/被禁用）：{e}")
        dev = next((d for d in devs if (d.name or "").lower() == DEVICE_NAME), None)
        if dev is None:
            named = [d for d in devs if d.name]
            print("未找到开发板。附近可见 BLE 设备：")
            for d in named[:15]:
                print(f"  {d.address}  {d.name}")

    if dev is None:
        print(f"\n回退：尝试已知地址 {KNOWN_ADDRESS} …")
        dev = await BleakScanner.find_device_by_address(KNOWN_ADDRESS, timeout=12)
    if dev is None:
        sys.exit("未找到开发板。请确认：①开发板上电（USB 或电池）；②在本机蓝牙范围内；③未被手机等占用。")

    # 自动重试：区分「偶发抖动」与「每次必断（多为供电不足）」
    all_lines = []
    for i in range(1, RETRIES + 1):
        res = await connect_once(dev, i)
        if res is None:
            await asyncio.sleep(2)
            continue
        all_lines = res
        if all_lines:
            break
        await asyncio.sleep(2)

    print(f"\n[总结] {RETRIES} 次尝试，最终收到 {len(all_lines)} 行状态。")
    if not all_lines:
        print("[结论] 每次都能连上但立即掉线、无任何回复 —— 高度疑似供电不足（连接态射频")
        print("        电流骤增导致 ESP32 复位）。请检查：电池电量 / USB 线材与端口（避开")
        print("        USB hub 与前置面板口，改用主板直连口或优质线）、外接电源功率。")
        print("        若把板子 USB 接到本机，我可抓 CDC 串口看复位横幅进一步坐实。")


async def main():
    address = sys.argv[1] if len(sys.argv) > 1 else None
    await run(address)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n已中止。")
