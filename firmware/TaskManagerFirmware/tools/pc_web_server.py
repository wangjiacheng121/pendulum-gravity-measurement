#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_web_server.py —— 电脑端 WebUI 后端（ble_cli.py / phyphox_stream.py 的网页等价物）

与手机端的关系
    前端复用同一个文件 ble_cli_web.html（UI 完全一致），差别只在传输层：
        手机：页面直连 Web Bluetooth（调 Android/系统蓝牙栈）
        电脑：本脚本用 bleak 接管 BLE，页面通过 SSE 收行、POST 发命令

启动
    python pc_web_server.py                 # 扫描并连接 pendulum-g，自动开浏览器
    python pc_web_server.py AA:BB:CC:DD:EE:FF   # 指定地址直连
    python pc_web_server.py --port 9000     # 换端口（默认 8777）
    python pc_web_server.py --no-browser    # 不自动开浏览器

依赖
    pip install -r requirements.txt         # 只需要 bleak；Excel/绘图都在浏览器里做

HTTP 接口（前端调用）
    GET  /api/ping     握手：命中即进入「后端模式」
    GET  /api/events   SSE，逐行推送板子回包与状态
    POST /api/send     {"msg":"0"}  向板子写一行
    POST /api/connect  {"all":bool,"last":bool}  扫描连接 / 全扫描 / 重连上次
    POST /api/disconnect
    POST /api/selftest 无板也能验证整条 UI 链路（注入脚本化菜单/波形/导出数据）
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
import sys
import webbrowser

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # 依赖缺失时给出明确指引，不抛栈
    sys.exit("缺少 bleak 库：请执行  pip install -r requirements.txt")

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "ble_cli_web.html")

# NUS（Nordic UART Service），与固件 fw_ble 一致
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # 板接收（写）
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # 板发送（notify）
DEVICE_NAME = "pendulum-g"

CHUNK = 20          # 单次写入上限（默认 MTU 23 → 20 字节），与网页版一致
SCAN_TIMEOUT = 5    # 扫描时长（秒）


# ============================ 广播中心 ============================
class Hub:
    """把板子回包 / 状态变化广播给所有已打开的页面。"""

    def __init__(self) -> None:
        self.subs: set[asyncio.Queue] = set()

    def subscribe(self) -> asyncio.Queue:
        q: asyncio.Queue = asyncio.Queue(maxsize=2000)
        self.subs.add(q)
        return q

    def unsubscribe(self, q: asyncio.Queue) -> None:
        self.subs.discard(q)

    def broadcast(self, ev: dict) -> None:
        for q in list(self.subs):
            try:
                q.put_nowait(ev)
            except asyncio.QueueFull:
                # 慢客户端（页面卡住/后台节流）：丢掉最旧一条再塞，保证新数据能进
                with contextlib.suppress(asyncio.QueueEmpty):
                    q.get_nowait()
                with contextlib.suppress(asyncio.QueueFull):
                    q.put_nowait(ev)

    def line(self, text: str) -> None:
        self.broadcast({"t": "line", "s": text})

    def state(self, **kw) -> None:
        self.broadcast({"t": "state", **kw})


# ============================ BLE 链路 ============================
class BleLink:
    def __init__(self, hub: Hub) -> None:
        self.hub = hub
        self.client: BleakClient | None = None
        self.addr: str | None = None
        self.name: str | None = None
        self._buf = ""
        self._busy = False

    # ---------- 连接 ----------
    async def connect(self, scan_all: bool = False, last: bool = False) -> None:
        if self._busy:
            self.hub.line("INFO: 正在连接中，请稍候…")
            return
        self._busy = True
        try:
            if self.client and self.client.is_connected:
                await self.disconnect(quiet=True)

            target = None
            if last and self.addr:                      # 重连上次设备：先按地址直连
                self.hub.line(f"正在按上次地址连接 {self.addr} …")
                with contextlib.suppress(Exception):
                    target = await BleakScanner.find_device_by_address(self.addr, timeout=10)
            if target is None:
                self.hub.line(f"正在扫描 {DEVICE_NAME} …（约 {SCAN_TIMEOUT} 秒）")
                devs = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
                named = [d for d in devs if d.name]
                target = next((d for d in named if (d.name or "").lower() == DEVICE_NAME), None)
                if target is None and scan_all and named:
                    target = named[0]                  # 全扫描模式：取第一个有名字的设备
                if target is None:
                    self.hub.line("未找到开发板。附近可见 BLE 设备：")
                    for d in named[:15]:
                        self.hub.line(f"  {d.address}  {d.name}")
                    self.hub.line("请确认开发板已上电、未被其它 App 占用；"
                                  "必要时用「重新扫描（全部设备）」再试。")
                    self.hub.state(connected=False)
                    return

            self.addr, self.name = target.address, (target.name or target.address)
            self.hub.line(f"连接 {self.addr} ({self.name}) …")
            self.client = BleakClient(target, disconnected_callback=self._on_disconnect)
            await self.client.connect()
            await self.client.start_notify(NUS_TX, self._on_notify)
            self.hub.state(connected=True, name=self.name)
            self.hub.line("已连接。所有操作在页面终端进行，菜单项/命令与 PC 版 ble_cli.py 一致。")
        except Exception as e:                          # 链路异常统一回报，不让服务崩
            self.hub.line(f"ERR: 连接失败：{e}")
            self.hub.state(connected=False)
            with contextlib.suppress(Exception):
                if self.client:
                    await self.client.disconnect()
            self.client = None
        finally:
            self._busy = False

    async def disconnect(self, quiet: bool = False) -> None:
        if self.client and self.client.is_connected:
            with contextlib.suppress(Exception):
                await self.client.disconnect()
        self.client = None
        if not quiet:
            self.hub.line("已断开。")
            self.hub.state(connected=False)

    def _on_disconnect(self, _client) -> None:
        # bleak 回调线程 → 切回事件循环广播（与网页版同一语义：开发板消失即提示重连）
        asyncio.get_running_loop().call_soon_threadsafe(self._dropped)

    def _dropped(self) -> None:
        self.client = None
        self.hub.line("ERR: [连接已断开] 请检查开发板电源后重新扫描连接。")
        self.hub.state(connected=False)

    def _on_notify(self, _ch, data: bytearray) -> None:
        # 通知可能半行到达，按行缓冲后再广播（前端也会二次按行解析，双保险）
        self._buf += bytes(data).decode("utf-8", errors="replace")
        *lines, self._buf = self._buf.split("\n")
        for ln in lines:
            ln = ln.strip()
            if ln:
                asyncio.get_running_loop().call_soon_threadsafe(self.hub.line, ln)

    # ---------- 发送 ----------
    async def send(self, msg: str) -> None:
        if not self.client or not self.client.is_connected:
            self.hub.line("ERR: 未连接（开发板可能已断开，请重新扫描连接）。")
            return
        data = (msg + "\n").encode("utf-8")
        try:
            for i in range(0, len(data), CHUNK):
                await self.client.write_gatt_char(NUS_RX, data[i:i + CHUNK])
                if len(data) > CHUNK:
                    await asyncio.sleep(0.015)
        except Exception as e:
            self.hub.line(f"ERR: 发送失败：{e}")


# ============================ 无板自检 ============================
SELFTEST_SCRIPT_NOTE = "自检数据由后端注入，仅验证 UI 链路，不代表真实测量"


async def selftest(link: BleLink) -> None:
    """注入脚本化数据：菜单 → 状态 → 波形 → 历史导出，验证页面全链路（无需硬件）。"""
    import math

    async def emit(line: str, delay: float = 0.03) -> None:
        link.hub.line(line)
        await asyncio.sleep(delay)

    await emit("INFO: ===== 服务端自检模式（无板也能验证页面功能） =====")
    await emit("INFO: " + SELFTEST_SCRIPT_NOTE)
    await emit("OK: 板载固件 v? / 空闲 / 程序 2 单摆测g")
    await emit("#STREAM BEGIN")
    L, g0, seq, n = 1.000, 9.800, 0, 0
    for i in range(60):
        seq += 1
        n += 1
        half = math.sqrt(L / g0) * (1.0 + 0.004 * math.sin(i / 3.0))  # 人为抖动
        T = 2 * half
        g_inst = 4 * math.pi ** 2 * L / (T * T)
        g_avg = g0 + 0.002 * math.cos(i / 7.0)
        await emit(f"#STREAM {seq},{int(half*1e6)},{T:.6f},{g_inst:.4f},{g_avg:.4f},{n},{L:.3f}", 0.02)
    await emit("#STREAM END")
    await emit("#EXPORT BEGIN,3")
    # 历史数据必须物理自洽（T=2π√(L/g)），否则 T²–L 拟合斜率 ≤0 会被判为不可拟合
    g0 = 9.800
    hist = []
    for i, L in enumerate((0.500, 0.800, 1.000), start=1):
        T = 2 * math.pi * math.sqrt(L / g0) * (1 + 0.0005 * (i - 2))  # 轻微测量抖动
        G = 4 * math.pi ** 2 * L / (T * T)
        hist.append(f"{i},2026-10-02 15:0{i},{L:.3f},{24*i},{T:.3f},{G:.3f},0.0{i}%")
    for row in hist:
        await emit(row, 0.05)
    await emit("#EXPORT END")
    await emit("INFO: 自检结束。Excel 应已自动下载，页面出现 T²–L 图（拟合 g 应≈9.80）。")


# ============================ 极简 HTTP / SSE ============================
def _http_head(status: str, ctype: str, extra: str = "") -> bytes:
    return (f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\n{extra}"
            f"Connection: close\r\n\r\n").encode()


class Server:
    def __init__(self, link: BleLink, hub: Hub, port: int) -> None:
        self.link, self.hub, self.port = link, hub, port

    async def handle(self, reader: asyncio.StreamReader,
                     writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            if not line:
                return
            parts = line.decode("latin1").split()
            if len(parts) < 2:
                return
            method, path = parts[0], parts[1].split("?")[0]

            headers: dict[str, str] = {}
            while True:
                h = await reader.readline()
                if h in (b"\r\n", b"\n", b""):
                    break
                k, _, v = h.decode("latin1").partition(":")
                headers[k.strip().lower()] = v.strip()

            body = b""
            if n := int(headers.get("content-length", 0) or 0):
                body = await reader.readexactly(n)

            if method == "GET" and path in ("/", "/index.html", "/ble_cli_web.html"):
                await self._send_page(writer)
            elif method == "GET" and path == "/api/ping":
                await self._json(writer, {"ok": True, "mode": "server",
                                          "ble": bool(self.link.client and self.link.client.is_connected)})
            elif method == "GET" and path == "/api/events":
                await self._sse(writer)
            elif method == "POST" and path == "/api/send":
                msg = str(self._body(body).get("msg", "")).strip()
                if msg:
                    self.hub.line("» " + msg)        # 与网页版一致：回显已发送内容
                    await self.link.send(msg)
                await self._json(writer, {"ok": True})
            elif method == "POST" and path == "/api/connect":
                opt = self._body(body)
                asyncio.create_task(self.link.connect(
                    scan_all=bool(opt.get("all")), last=bool(opt.get("last"))))
                await self._json(writer, {"ok": True})
            elif method == "POST" and path == "/api/disconnect":
                await self.link.disconnect()
                await self._json(writer, {"ok": True})
            elif method == "POST" and path == "/api/selftest":
                asyncio.create_task(selftest(self.link))
                await self._json(writer, {"ok": True})
            else:
                writer.write(_http_head("404 Not Found", "text/plain; charset=utf-8"))
                await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionResetError):
            pass
        except Exception as e:                        # 单个请求出错不影响服务
            self.hub.line(f"ERR: 服务内部错误：{e}")
        finally:
            with contextlib.suppress(Exception):
                writer.close()
                await writer.wait_closed()

    # ---- 工具 ----
    @staticmethod
    def _body(raw: bytes) -> dict:
        with contextlib.suppress(Exception):
            return json.loads(raw.decode("utf-8") or "{}")
        return {}

    async def _json(self, writer: asyncio.StreamWriter, obj: dict) -> None:
        writer.write(_http_head("200 OK", "application/json; charset=utf-8",
                                "Content-Length: %d\r\n" % len(json.dumps(obj).encode()))
                     + json.dumps(obj).encode())
        await writer.drain()

    async def _send_page(self, writer: asyncio.StreamWriter) -> None:
        if not os.path.exists(PAGE):
            writer.write(_http_head("500 Internal Server Error", "text/plain; charset=utf-8",
                                    "Content-Length: 0\r\n"))
            await writer.drain()
            self.hub.line("ERR: 找不到 ble_cli_web.html（应与本脚本同目录）")
            return
        data = open(PAGE, "rb").read()
        writer.write(_http_head("200 OK", "text/html; charset=utf-8",
                                "Content-Length: %d\r\n" % len(data))
                     + data)
        await writer.drain()

    async def _sse(self, writer: asyncio.StreamWriter) -> None:
        q = self.hub.subscribe()
        writer.write(_http_head("200 OK", "text/event-stream; charset=utf-8",
                                "Cache-Control: no-cache\r\nX-Accel-Buffering: no\r\n"))
        await writer.drain()
        try:
            first = {"t": "state", "connected": bool(self.link.client and self.link.client.is_connected),
                     "name": self.link.name or "—", "mode": "server"}
            writer.write(f"data: {json.dumps(first)}\n\n".encode())
            await writer.drain()
            while True:
                try:
                    ev = await asyncio.wait_for(q.get(), timeout=15)
                except asyncio.TimeoutError:
                    writer.write(b": keepalive\n\n")     # 保活，防代理断连
                    await writer.drain()
                    continue
                writer.write(f"data: {json.dumps(ev, ensure_ascii=False)}\n\n".encode())
                await writer.drain()
        except (ConnectionResetError, BrokenPipeError, asyncio.CancelledError):
            pass
        finally:
            self.hub.unsubscribe(q)


# ============================ 入口 ============================
async def main_async(addr: str | None, port: int) -> None:
    hub = Hub()
    link = BleLink(hub)
    if addr:
        link.addr = addr.upper()
    srv = await asyncio.start_server(Server(link, hub, port).handle, "127.0.0.1", port)
    url = f"http://127.0.0.1:{port}/"
    print("=" * 58)
    print(" pendulum-g 电脑端 WebUI 已启动")
    print(f" 请在浏览器打开：{url}")
    print(" 若开发板还没上电：先上电，再点页面上的「连接 pendulum-g」")
    print(" 无板验证页面功能：点页面上的「服务自检」按钮")
    print(" 停止服务：Ctrl+C")
    print("=" * 58)
    asyncio.create_task(link.connect(last=bool(addr)))
    async with srv:
        await srv.serve_forever()


def main() -> None:
    ap = argparse.ArgumentParser(description="pendulum-g 电脑端 WebUI（ble_cli.py 网页版）")
    ap.add_argument("address", nargs="?", help="开发板 BLE 地址（可选，省略则按名字扫描）")
    ap.add_argument("--port", type=int, default=8777, help="本地端口（默认 8777）")
    ap.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    args = ap.parse_args()
    try:
        asyncio.run(main_async(args.address, args.port))
    except KeyboardInterrupt:
        print("\n已退出。")


if __name__ == "__main__":
    main()
