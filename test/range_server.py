#!/usr/bin/env python3
"""range_server.py — pjfetch 本地测试服务器（Python 3 标准库，无第三方依赖）。

内容模型（必须与 make_testfile.py 完全一致，两处代码同步修改）：
  种子 --seed 的 32 位 LCG（x = A*x + C mod 2^32，A=1103515245，C=12345）
  预生成 BLOCK=1MiB 字节表；逻辑流 = 该表周期重复：byte[pos] = table[pos % BLOCK]。
  （周期化是为了 O(1) 随机寻址 + 纯 stdlib 高速切片；哈希比对双方同构即可）

行为：
  - 无 Range / 不认识的 Range -> 200 全量（带 Content-Length）
  - 单段 "bytes=a-b"（也接受 "a-" / "-N"；逗号多段不支持 -> 200 全量）
      命中 -> 206 + Content-Range: bytes a-b/SIZE
      start 超界 -> 416 + Content-Range: bytes */SIZE
  - --size N         逻辑流总长（默认 1MiB）
  - --seed S         内容种子（默认 42）
  - --throttle KB/s  限速逐块发送且不发 Content-Length（未知大小），忽略 Range
  - --no-range       忽略 Range，恒 200
  - --redirect       GET/HEAD 先 302 到同端口 /real
  - --status N       所有请求恒返回状态码 N（含 HEAD；用于 404/500 等错误路径）
  - --collapse       首次 Range（探测 bytes=0-0）如实返回 206，其后的 Range 一律
                     返回 200 全量——模拟"宣称支持 Range 却不兑现"的服务器
  - --kill-after N   发送 N 字节后直接断开连接（模拟传输中断，用于重试测试）
  - --port P --host H（默认 127.0.0.1:8000）
每个请求打印 "METHOD PATH range=... -> CODE bytes=N" 到 stdout（flush），供核对
"≥8 个 Range 请求"。Ctrl+C 干净退出。

用法示例：
  python test/range_server.py --size 104857600 --port 8000
  python test/range_server.py --throttle 512          # 512KiB/s，未知大小
"""
import argparse
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BLOCK = 1 << 20          # LCG 表长 1MiB（内容流周期）
A, C, M = 1103515245, 12345, 1 << 32
CHUNK = 64 * 1024        # 发送切片


def lcg_table(nbytes, seed):
    """LCG 预生成 nbytes 字节表；byte_i = (x_i >> 16) & 0xFF。"""
    x = seed & 0xFFFFFFFF
    out = bytearray(nbytes)
    for i in range(nbytes):
        x = (A * x + C) % M
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


class RangeServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, handler, *, size, seed, throttle, no_range, redirect,
                 status=0, collapse=False, kill_after=0):
        super().__init__(addr, handler)
        self.size = size
        self.seed = seed
        self.throttle = throttle       # KiB/s，None = 不限速
        self.no_range = no_range
        self.redirect = redirect
        self.status = status           # 非 0 = 所有请求恒返回该状态码
        self.collapse = collapse
        self.kill_after = kill_after   # >0 = 发送该字节数后断连
        self.table = lcg_table(BLOCK, seed)
        self.lock = threading.Lock()
        self.range_seen = 0            # collapse 模式：已如实服务过的 Range 请求数


class RangeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "pjrange/1.0"

    # ---- 静音默认日志（我们自己打印单行摘要） ----
    def log_message(self, fmt, *args):
        pass

    def do_HEAD(self):
        self._handle(send_body=False)

    def do_GET(self):
        self._handle(send_body=True)

    # ---- --kill-after：首个带 body 的响应写到 N 字节即粗暴断连（模拟传输中断） ----
    def send_body(self, start, length):
        """按 kill_after 决定是否中途断连；返回实际发送字节数。"""
        srv = self.server
        with srv.lock:
            armed = srv.kill_after > 0
            n_kill = srv.kill_after
            if armed:
                srv.kill_after = 0        # 只中断一次：重试即可成功
        if not armed or n_kill >= length:
            self.stream_content(start, length)
            return length
        self.stream_content(start, n_kill)
        try:
            self.wfile.flush()
        except Exception:
            pass
        # 声明了完整 Content-Length 却提前关闭 → 客户端应看到截断（PARTIAL_FILE/RECV_ERROR）
        self.close_connection = True
        try:
            self.connection.shutdown(2)
        except Exception:
            pass
        self._log(self.path, "kill-after=%d" % n_kill, 200, n_kill)
        return n_kill

    # ---- 内容寻址切片（周期表） ----
    def stream_content(self, start, length):
        t = self.server.table
        b = len(t)
        pos, remaining, w = start, length, self.wfile
        while remaining > 0:
            p = pos % b
            take = min(remaining, b - p, CHUNK)
            w.write(t[p:p + take])
            remaining -= take
            pos += take

    # ---- Range 解析：None=无/不认识(200 全量)，"over"=416，(a,b0 inclusive) ----
    def parse_range(self, size):
        raw = self.headers.get("Range")
        if not raw:
            return None
        raw = raw.strip()
        if not raw.startswith("bytes="):
            return None
        spec = raw[len("bytes="):]
        if "," in spec:                       # 多段不支持
            return None
        a, _, b = spec.partition("-")
        a, b = a.strip(), b.strip()
        try:
            if a == "":                       # -N 后缀
                if int(b) <= 0:
                    return None
                start = max(0, size - int(b))
                return (start, size - 1)
            start = int(a)
            if start >= size:
                return "over"
            if b == "":                       # a-
                return (start, size - 1)
            end = min(int(b), size - 1)
            if start > end:
                return None
            return (start, end)
        except ValueError:
            return None

    def _handle(self, send_body):
        srv = self.server

        # --status：所有请求（含 HEAD）恒返回指定状态码
        if srv.status:
            self.send_response(srv.status)
            self.send_header("Content-Length", "0")
            self.end_headers()
            self._log(self.path, "-", srv.status, 0)
            return

        # --redirect：先 302 到 /real（/real 本身放行）
        if srv.redirect and not self.path.startswith("/real"):
            self.send_response(302)
            self.send_header("Location", "/real")
            self.send_header("Content-Length", "0")
            self.end_headers()
            self._log(self.path, "-", 302, 0)
            return

        rng = None if (srv.no_range or srv.throttle) else self.parse_range(srv.size)

        # --collapse：探测用的 bytes=0-0 如实 206；其后的 Range 一律降级为 200 全量
        if srv.collapse and rng not in (None, "over"):
            is_probe = (rng[0] == 0 and rng[1] == 0)
            with srv.lock:
                seen = srv.range_seen
                if not is_probe:
                    srv.range_seen += 1
            if not is_probe and seen >= 1:
                rng = None                # 降级：忽略 Range，返回 200 全量


        if rng == "over":                     # 416
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % srv.size)
            self.send_header("Content-Length", "0")
            self.end_headers()
            self._log(self.path, "invalid", 416, 0)
            return

        if rng is None:                       # 200 全量
            if srv.throttle:
                self._send_streaming(send_body)
            else:
                self.send_response(200)
                self.send_header("Content-Length", str(srv.size))
                self.end_headers()
                if send_body:
                    self.send_body(0, srv.size)
                self._log(self.path, "-", 200, srv.size)
            return

        start, end = rng                      # 206
        n = end - start + 1
        self.send_response(206)
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, srv.size))
        self.send_header("Content-Length", str(n))
        self.end_headers()
        if send_body:
            self.send_body(start, n)
        self._log(self.path, "bytes=%d-%d/%d" % (start, end, srv.size), 206, n)

    def _send_streaming(self, send_body):
        """限速流式：无 Content-Length（未知大小），Connection: close 结尾。"""
        rate = self.server.throttle * 1024.0   # B/s
        self.send_response(200)
        self.send_header("Connection", "close")
        self.close_connection = True
        self.end_headers()
        n = 0
        if send_body:
            chunk = 1024
            delay = chunk / rate
            t = self.server.size
            sent = 0
            while sent < t:
                take = min(chunk, t - sent)
                self.stream_content(sent, take)
                sent += take
                n += take
                if sent < t:
                    time.sleep(delay)
        self._log(self.path, "throttle", 200, n)

    def _log(self, path, rng, code, n):
        rngs = rng if rng != "-" else "-"
        print("%s %s range=%s -> %d bytes=%d"
              % (self.command, path, rngs, code, n), flush=True)


def main():
    ap = argparse.ArgumentParser(description="pjfetch local range test server")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=1 << 20,
                    help="content length in bytes (default 1MiB)")
    ap.add_argument("--seed", type=int, default=42, help="LCG seed (default 42)")
    ap.add_argument("--throttle", type=float, default=None, metavar="KB/s",
                    help="throttle send rate; implies unknown size, ignores Range")
    ap.add_argument("--no-range", action="store_true", help="ignore Range, always 200")
    ap.add_argument("--redirect", action="store_true", help="302 / -> /real first")
    ap.add_argument("--status", type=int, default=0,
                    help="always respond with this status code (e.g. 404, 500)")
    ap.add_argument("--collapse", action="store_true",
                    help="first Range (probe) is 206, later Ranges degrade to 200")
    ap.add_argument("--kill-after", type=int, default=0, metavar="N",
                    help="abort the first body response after N bytes (one-shot)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--ttl", type=float, default=600.0, metavar="SECONDS",
                    help="最长存活秒数，到点自退（避免测试异常退出后留下僵尸服务器）")
    ap.add_argument("--log", default=None, metavar="FILE",
                    help="把自身输出写入该文件（测试夹具以此避免依赖父进程的句柄重定向）")
    args = ap.parse_args()

    # --log：自行落盘。测试夹具以"不继承句柄"的方式分离启动服务器，
    # 若靠父进程 shell 重定向，子进程会继承并长期持有父进程的输出管道，
    # 导致 ctest 之类的捕获式运行器永远等不到管道 EOF 而挂死。
    if args.log:
        _f = open(args.log, "a", buffering=1, encoding="utf-8", errors="replace")
        sys.stdout = _f
        sys.stderr = _f

    srv = RangeServer((args.host, args.port), RangeHandler,
                      size=args.size, seed=args.seed, throttle=args.throttle,
                      no_range=args.no_range, redirect=args.redirect,
                      status=args.status, collapse=args.collapse,
                      kill_after=args.kill_after)
    host, port = srv.server_address[:2]
    shown = host if isinstance(host, str) else host.decode()
    print("range_server on %s:%d size=%d seed=%d throttle=%s no_range=%s redirect=%s"
          " status=%s collapse=%s kill_after=%s"
          % (shown, port, args.size, args.seed,
             args.throttle, args.no_range, args.redirect,
             args.status, args.collapse, args.kill_after), flush=True)
    # 看门狗：到 TTL 自行退出，保证不留僵尸进程
    if args.ttl > 0:
        def _watchdog():
            time.sleep(args.ttl)
            print("range_server ttl expired, exiting", flush=True)
            os._exit(0)
        threading.Thread(target=_watchdog, daemon=True).start()

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
        print("range_server stopped", flush=True)


if __name__ == "__main__":
    main()
