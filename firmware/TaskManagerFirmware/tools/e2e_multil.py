# -*- coding: utf-8 -*-
"""e2e_multil.py —— 第二摆长组测量 + T²-L 拟合验证（真实板端数据）"""
import asyncio, sys, io, os, time
from datetime import datetime
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bleak import BleakScanner, BleakClient
import csv, e2e_sim_test

NUS_TX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
NUS_RX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
NEW_L = "1.444"

async def main():
    dev = await BleakScanner.find_device_by_name("pendulum-g", timeout=15)
    if dev is None:
        print("未找到板子"); return 2
    c = e2e_sim_test.Collector()
    async with BleakClient(dev, timeout=20) as client:
        await client.start_notify(NUS_RX, lambda ch, d: [c.feed(l) for l in d.decode("utf-8","replace").splitlines()])
        await asyncio.sleep(5.0)   # 等连接参数生效
        n0 = len(c.lines)

        async def send(s):
            await client.write_gatt_char(NUS_TX, (s+"\n").encode())
            print(">>", s)

        async def wait_reply(key, timeout=8.0):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if any(key in l for l in c.lines[n0:]): return True
                await asyncio.sleep(0.3)
            return False

        # 0) 切到程序2（刷机后默认在桌宠）
        await send("APP 2");  await asyncio.sleep(3.0)

        # 1) 改摆长 1.444m（含 y 确认）
        await send("2");      await asyncio.sleep(2.0)
        await send("2.2");    await asyncio.sleep(2.0)
        await send(NEW_L);    await asyncio.sleep(2.0)
        await send("y");      await asyncio.sleep(2.0)
        await send("q");      await asyncio.sleep(2.0)
        ok_l = any(l.startswith("OK") and NEW_L in l for l in c.lines[n0:]) or \
               any("L=" + NEW_L in l and "确认" not in l for l in c.lines[n0:])
        print("改L:", "OK" if ok_l else "未确认")

        # 2) SIM ON → 测 16s → SIM OFF（结算写入历史）
        await send("SIM ON"); await asyncio.sleep(3.0)
        print("SIM:", "OK" if any("模拟数据已开启" in l for l in c.lines[n0:]) else "未确认")
        await asyncio.sleep(16.0)
        await send("SIM OFF"); await asyncio.sleep(3.0)

        # 3) 导出
        await send("3");      await asyncio.sleep(2.0)
        await send("3.2")
        t0 = time.time()
        while not c.got["export"] and time.time() - t0 < 20:
            await asyncio.sleep(0.5)
    await asyncio.sleep(0.5)

    rows = c.export_rows
    print(f"导出 {len(rows)} 行：")
    for r in rows: print("  ", r)

    if rows:
        import ble_cli
        cli = ble_cli.BleCli()
        out = os.path.join(HERE, "_selftest_out")
        os.makedirs(out, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        cli._write_and_plot(rows, os.path.join(out, f"multi_history_{stamp}.xlsx"),
                                 os.path.join(out, f"multi_T2L_{stamp}.png"))
        ls = {r[2] for r in rows if len(r) > 2}
        print("摆长组数:", len(ls), ls)
    return 0

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
