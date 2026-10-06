#!/usr/bin/env python3
# ============================================================
# 告警接收端(演示/联调用) —— 只依赖标准库
# ------------------------------------------------------------
# 用途: 把 config 里 alert.push.url 指过来, 就能看见"告警真的出去了",
#       以及推送重试/退避/队列丢包这些行为在接收端长什么样。
#
# 用法:
#     python3 scripts/alert_receiver.py [--port 8899] [--token <x-alert-token>]
#     # 然后 config 里:
#     #   alert.push.enabled: true
#     #   alert.push.url: "http://127.0.0.1:8899/alert"
#     #   alert.push.header_value: "<同一个 token>"   (不设 token 则不校验)
#
# 退出: Ctrl+C(或 kill -TERM)
# 说明: 这是**开发/演示**用的最小接收端 —— 单线程串行 + sleep 模拟下游处理,
#       故意不做持久化/鉴权加固。生产请用真实的通知系统(钉钉/飞书/Slack/自建)。
# ============================================================
import argparse
import json
import signal
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_received = 0


class Handler(BaseHTTPRequestHandler):
    server_version = "alert-receiver/1.0"
    expected_token = ""
    delay_ms = 0
    fail_first = 0          # 前 N 次故意返回 500(演示"重试+退避")
    _calls = 0

    # ---- 让日志看着像日志, 而不是 python 的默认格式 ----
    def log_message(self, fmt, *args):
        sys.stderr.write("[receiver] %s - %s\n" % (self.address_string(), fmt % args))

    def _reply(self, code: int, body: str = ""):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if data:
            self.wfile.write(data)

    def do_POST(self):  # noqa: N802 (stdlib 命名)
        global _received
        if self.path.split("?")[0] != "/alert":
            self._reply(404, '{"error":"not found"}')
            return

        if Handler.expected_token:
            got = self.headers.get("x-alert-token", "")
            if got != Handler.expected_token:
                # 和 C++ 侧同一条纪律: 拒绝要留痕
                sys.stderr.write("[receiver] 拒绝: x-alert-token 不匹配 (got=%r)\n" % got)
                self._reply(401, '{"error":"unauthorized"}')
                return

        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            payload = json.loads(raw.decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write("[receiver] body 不是合法 JSON: %s\n" % exc)
            self._reply(400, '{"error":"bad json"}')
            return

        Handler._calls += 1
        if Handler.fail_first >= Handler._calls:
            sys.stderr.write("[receiver] 故意失败一次(演示重试): call=%d\n" % Handler._calls)
            self._reply(500, '{"error":"injected failure"}')
            return

        _received += 1
        # 有效载荷契约: {source, alert_type, description, frame_seq, label,
        #                   confidence, track_id, ts_ms}; 兼容读 type 字段
        alert_type = payload.get("alert_type") or payload.get("type") or "?"
        # 一行一条, 方便 grep / 计数
        print("[receiver] #%d %s | %s | frame=%s label=%s conf=%s track=%s | total_received=%d"
              % (_received, alert_type, payload.get("description"),
                 payload.get("frame_seq"), payload.get("label"),
                 payload.get("confidence"), payload.get("track_id"), _received),
              flush=True)

        if Handler.delay_ms:
            time.sleep(Handler.delay_ms / 1000.0)

        self._reply(200, json.dumps({"ok": True, "received": _received}))


def main():
    ap = argparse.ArgumentParser(description="告警接收端(演示用)")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--token", default="", help="校验 x-alert-token(空 = 不校验)")
    ap.add_argument("--delay-ms", type=int, default=0, help="每条告警的处理耗时(演示队列丢包)")
    ap.add_argument("--fail-first", type=int, default=0, help="前 N 次故意返回 500")
    args = ap.parse_args()

    Handler.expected_token = args.token
    Handler.delay_ms = args.delay_ms
    Handler.fail_first = args.fail_first

    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    srv.daemon_threads = True

    def _stop(_signum, _frame):
        print("\n[receiver] 收到退出信号, 共收到 %d 条告警" % _received, flush=True)
        # 交给主线程收尾(避免在信号处理里做重活)
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    print("[receiver] 监听 http://%s:%d/alert%s"
          % (args.bind, args.port, "(需要 x-alert-token)" if args.token else ""), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
