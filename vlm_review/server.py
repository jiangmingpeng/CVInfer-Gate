#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Phase C 复核服务端(真 VLM 接入) —— gRPC 入口.

用法:
    # 1) 指向任意 OpenAI 兼容多模态服务(vLLM/Ollama/LM Studio/DashScope/OpenAI)
    VLM_BASE_URL=http://127.0.0.1:8000/v1 VLM_MODEL=Qwen/Qwen2.5-VL-7B-Instruct \
        python3 -m vlm_review.server --port 50052

    # 2) 本地 transformers(Qwen2.5-VL)
    python3 -m vlm_review.server --backend transformers \
        --model Qwen/Qwen2.5-VL-7B-Instruct

    # 3) 确定性 mock(自检/CI, 与旧 mock 行为一致)
    python3 -m vlm_review.server --backend mock

随后用联调客户端验证:
    ./build/review_client 127.0.0.1:50052 frame.jpg "判断该人员是否未佩戴安全帽"

再把 C++ 侧 config.yaml 的 review.endpoint 指到本服务即可(流水线零改动)。

    # 4) [T41] 开启鉴权(与 C++ 侧 review.auth_token 对称): 服务端
    VLM_AUTH_TOKEN=xxx python3 -m vlm_review.server --backend mock
    #    同一 token 丢给客户端(环境变量优先, 不写进命令行历史)
    REVIEW_AUTH_TOKEN=xxx ./build/review_client 127.0.0.1:50052
    注: 开启后 Health 也要带 token; token 为空则完全不校验(零破坏)。
"""
from __future__ import annotations

import argparse
import hmac
import os
import subprocess
import sys
import threading
import time
from concurrent import futures

from .backends import BackendError, build_backend
from .config import VlmConfig


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ensure_proto(pb2_dir: str, proto_root: str) -> None:
    """确保 review_pb2.py / review_pb2_grpc.py 存在; 缺失则现场生成(免手工步骤)."""
    os.makedirs(pb2_dir, exist_ok=True)
    need = (not os.path.isfile(os.path.join(pb2_dir, "review_pb2.py"))
            or not os.path.isfile(os.path.join(pb2_dir, "review_pb2_grpc.py")))
    if need:
        print(f"[vlm] 未找到生成代码, 正在编译 {proto_root}/review.proto -> {pb2_dir} ...",
              flush=True)
        subprocess.check_call([
            sys.executable, "-m", "grpc_tools.protoc",
            f"-I{proto_root}",
            f"--python_out={pb2_dir}",
            f"--grpc_python_out={pb2_dir}",
            os.path.join(proto_root, "review.proto"),
        ])
    if pb2_dir not in sys.path:
        sys.path.insert(0, pb2_dir)


def build_parser(cfg: VlmConfig) -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="CVInfer-Gate 复核服务端 (Phase C, 真 VLM)")
    ap.add_argument("--host", default=cfg.host, help=f"监听地址(默认 {cfg.host})")
    ap.add_argument("--port", type=int, default=cfg.port,
                    help=f"监听端口(需与 review.endpoint 一致, 默认 {cfg.port})")
    ap.add_argument("--backend", default=cfg.backend,
                    choices=["openai", "transformers", "mock"], help="复核后端")
    ap.add_argument("--model", default=cfg.model, help="模型名(空 = 后端默认)")
    ap.add_argument("--base-url", dest="base_url", default=cfg.base_url,
                    help="openai 后端: OpenAI 兼容 /v1 地址")
    ap.add_argument("--api-key", dest="api_key", default=cfg.api_key, help="openai 后端: API Key")
    ap.add_argument("--max-workers", dest="max_workers", type=int, default=cfg.max_workers,
                    help="gRPC 线程池大小")
    ap.add_argument("--max-tokens", dest="max_tokens", type=int, default=cfg.max_tokens)
    ap.add_argument("--auth-token", dest="auth_token", default=cfg.auth_token,
                    help="非空 = 要求调用方带 authorization: Bearer <token>(空 = 不校验)")
    ap.add_argument("--request-timeout-s", dest="request_timeout_s", type=float,
                    default=cfg.request_timeout_s, help="单次上游 HTTP 超时(秒)")
    ap.add_argument("--pb2-dir", dest="pb2_dir", default=cfg.pb2_dir,
                    help="protoc 生成代码目录(空 = <repo>/build/pyproto)")
    ap.add_argument("--max-rpcs", dest="max_rpcs", type=int, default=0,
                    help="处理满 N 个请求后自动退出(0 = 不退出)")
    return ap


class _AuthInterceptor:
    """[T41] 服务端鉴权: 校验 authorization: Bearer <token>。

    与 C++ 侧**对称**: GrpcLlmReviewer 在 auth_token 非空时对**每个** RPC(含 Health)
    都发 `authorization: Bearer <token>`(见 src/review/GrpcLlmReviewer.cpp)。
    所以这里也对**所有**方法统一校验, 不做例外 —— 语义最简单, 也堵住了
    "将来新增 RPC 忘了鉴权"这个最常见的漏洞(新增方法自动被覆盖)。

    * token 为空 => 不启用鉴权(完全等价于 T41 之前的行为, 零破坏);
    * 用 hmac.compare_digest 做**定长比较**, 避免按字节短路泄漏 token 前缀;
    * 注: 这是**明文传输的共享密钥**(不启用 TLS), 只解决"谁都能调"的问题,
      不解决窃听 —— 内网/本地回环够用, 公网必须上 TLS 或反向代理。

    为何**不继承** grpc.ServerInterceptor: 本模块是延迟导入 grpc 的(未装 grpcio
    时给友好提示), 而基类在**类定义时**就要解析 grpc; 而 grpc.server 对拦截器
    只鸭子调用 intercept_service(), 继承仅用于类型标注。
    """

    def __init__(self, token: str, grpc_mod):
        self._expected = "Bearer " + token
        self._grpc = grpc_mod
        self._denied = 0   # 拒绝次数(退出/统计时可读)

    @property
    def denied(self) -> int:
        """被拒绝的调用次数(观测用: 突增说明有人在试 token, 或客户端配错了)。"""
        return self._denied

    def _deny(self, context):
        self._denied += 1
        context.abort(
            self._grpc.StatusCode.UNAUTHENTICATED,
            "缺少或错误的 authorization metadata (应为: Bearer <token>)",
        )

    def intercept_service(self, continuation, handler_call_details):
        handler = continuation(handler_call_details)
        if handler is None:
            return None
        got = ""
        for key, value in (handler_call_details.invocation_metadata or ()):
            if key == "authorization":
                got = value or ""
                break
        try:
            ok = hmac.compare_digest(got, self._expected)
        except TypeError:
            # 非 ASCII metadata 会让 compare_digest 抛 TypeError: 必须吃掉它,
            # 否则状态码会从 UNAUTHENTICATED 变成 UNKNOWN(等于否认"鉴权失败")
            ok = False
        if ok:
            return handler
        # 拒绝: 返回与原型**同类型**的"直接 abort"实现
        deny = lambda req, ctx: self._deny(ctx)  # noqa: E731
        if handler.unary_unary:
            return self._grpc.unary_unary_rpc_method_handler(
                deny, request_deserializer=handler.request_deserializer,
                response_serializer=handler.response_serializer)
        if handler.unary_stream:
            return self._grpc.unary_stream_rpc_method_handler(
                deny, request_deserializer=handler.request_deserializer,
                response_serializer=handler.response_serializer)
        # 流式 RPC(本项目未用): 一律拒绝, 不留下"漏校验"的通道
        return self._grpc.unary_unary_rpc_method_handler(deny)


def serve(cfg: VlmConfig, max_rpcs: int = 0) -> int:
    root = _repo_root()
    proto_root = os.path.join(root, "proto")
    if not os.path.isfile(os.path.join(proto_root, "review.proto")):
        print(f"[vlm] 找不到 {proto_root}/review.proto", file=sys.stderr)
        return 2

    pb2_dir = cfg.pb2_dir or os.path.join(root, "build", "pyproto")
    try:
        ensure_proto(pb2_dir, proto_root)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"[vlm] 生成 proto 失败: {e}", file=sys.stderr)
        print("     请先: pip install grpcio grpcio-tools", file=sys.stderr)
        return 2

    try:
        import grpc
        import review_pb2 as pb
        import review_pb2_grpc as pb_grpc
    except ImportError as e:
        print(f"[vlm] 导入 gRPC/proto 模块失败: {e}", file=sys.stderr)
        print("     请先: pip install -r vlm_review/requirements.txt", file=sys.stderr)
        return 2

    backend = build_backend(cfg)
    ready, detail = backend.ready()
    if not ready:
        print(f"[vlm] 警告: 后端未就绪({detail}); 仍会启动, 但复核可能失败。", file=sys.stderr)

    # 并发安全计数(grpc.server 用线程池并发收请求, 用锁避免编号竞态)
    state = {"n": 0}
    state_lock = threading.Lock()

    def next_no() -> int:
        with state_lock:
            state["n"] += 1
            return state["n"]

    class ReviewServicer(pb_grpc.ReviewServiceServicer):
        def Review(self, request, context):  # noqa: N802 (protoc 生成名)
            n = next_no()
            seq = int(request.frame_seq)
            jpeg = bytes(request.image_jpeg)
            prompt = request.prompt or cfg.default_prompt

            if not jpeg:
                context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
                context.set_details("image_jpeg 为空")
                return pb.ReviewResponse(ok=False, model=backend.model_name,
                                         reason="image_jpeg 为空")

            t0 = time.perf_counter()
            try:
                verdict = backend.infer(jpeg, request.label, float(request.confidence),
                                        prompt, seq)
            except BackendError as e:
                code = (grpc.StatusCode.UNAVAILABLE if e.unavailable
                        else grpc.StatusCode.INTERNAL)
                context.set_code(code)
                context.set_details(str(e))
                print(f"[vlm] #{n:<4} seq={seq:<6} label={request.label:<8} "
                      f"-> ERROR({code.name}): {e}", flush=True)
                return pb.ReviewResponse(ok=False, model=backend.model_name, reason=str(e))
            except Exception as e:  # noqa: BLE001  (后端异常一律收敛, 不让 gRPC 裸抛)
                context.set_code(grpc.StatusCode.INTERNAL)
                context.set_details(f"backend exception: {e}")
                print(f"[vlm] #{n:<4} seq={seq:<6} -> EXCEPTION: {e}", flush=True)
                return pb.ReviewResponse(ok=False, model=backend.model_name, reason=str(e))

            latency_ms = int((time.perf_counter() - t0) * 1000)
            if cfg.log_every > 0 and (n % cfg.log_every == 0):
                print(f"[vlm] #{n:<4} seq={seq:<6} label={request.label:<8} "
                      f"conf={request.confidence:.2f} roi={request.roi_meta:<12} "
                      f"-> confirmed={verdict.confirmed} ({verdict.label}, "
                      f"{verdict.confidence:.2f}) {latency_ms}ms", flush=True)
            return pb.ReviewResponse(ok=True, confirmed=verdict.confirmed,
                                     label=verdict.label, confidence=verdict.confidence,
                                     reason=verdict.reason, model=backend.model_name,
                                     latency_ms=latency_ms)

        def Health(self, request, context):  # noqa: N802
            ok, msg = backend.ready()
            return pb.HealthResponse(ready=ok, backend=backend.kind,
                                     model=backend.model_name, detail=msg)

    interceptors = []
    if cfg.auth_token:
        interceptors.append(_AuthInterceptor(cfg.auth_token, grpc))
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=cfg.max_workers),
                         interceptors=interceptors)
    pb_grpc.add_ReviewServiceServicer_to_server(ReviewServicer(), server)
    addr = f"{cfg.host}:{cfg.port}"
    server.add_insecure_port(addr)
    server.start()

    # 注意: 每行都 flush=True —— 输出被重定向到文件/管道时 stdout 是块缓冲的,
    #       不 flush 会导致"服务已起但仍看不到日志"的排查噩梦(已踩过)。
    print(f"[vlm] 复核服务已启动: {addr}", flush=True)
    print(f"[vlm] backend={backend.kind} model={backend.model_name} | {detail}", flush=True)
    # [T41] 鉴权状态必须打出来: "以为开了其实没开"是排查噩梦
    if cfg.auth_token:
        print("[vlm] 鉴权: 已开启(要求 authorization: Bearer <token>; Health 也要带)", flush=True)
    else:
        print("[vlm] 鉴权: 未开启(任何调用方都能访问; 设 VLM_AUTH_TOKEN 开启)", flush=True)
    if backend.kind == "openai":
        print(f"[vlm] 上游: {cfg.base_url}/chat/completions "
              f"(timeout={cfg.request_timeout_s}s, max_tokens={cfg.max_tokens})", flush=True)
    print("[vlm] 等待 CVInfer-Gate / review_client 的复核请求... 按 Ctrl+C 退出", flush=True)

    try:
        while True:
            time.sleep(1.0)
            if max_rpcs > 0 and state["n"] >= max_rpcs:
                print(f"[vlm] 已处理 {state['n']} 个请求(--max-rpcs), 退出。", flush=True)
                break
    except KeyboardInterrupt:
        print("\n[vlm] 收到中断, 正在停止...")

    server.stop(0)
    return 0


def main(argv=None) -> int:
    cfg = VlmConfig.from_env()
    args = build_parser(cfg).parse_args(argv)
    # CLI 覆盖环境变量
    cfg.host = args.host
    cfg.auth_token = args.auth_token
    cfg.port = args.port
    cfg.backend = args.backend
    cfg.model = args.model
    cfg.base_url = args.base_url
    cfg.api_key = args.api_key
    cfg.max_workers = args.max_workers
    cfg.max_tokens = args.max_tokens
    cfg.request_timeout_s = args.request_timeout_s
    cfg.pb2_dir = args.pb2_dir
    return serve(cfg, max_rpcs=args.max_rpcs)


if __name__ == "__main__":
    raise SystemExit(main())
