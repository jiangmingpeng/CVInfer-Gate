#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================
# Phase C 复核服务 Mock (T20-T22)
# ------------------------------------------------------------
# 用途: 本仓库只实现复核**客户端**(GrpcLlmReviewer + proto/review.proto),
#       服务端由"部署了大模型/VLM 的进程"提供。为了让 Phase C 的
#       "确认才告警 / 否决不告警 / 不可用兜底"三条分支都能被端到端看到,
#       这里提供一个轻量 mock(Python), 按规则返回结论。
#
# 前置(一次性):
#     pip install grpcio grpcio-tools
#     mkdir -p build/pyproto
#     python3 -m grpc_tools.protoc -I proto \
#         --python_out=build/pyproto --grpc_python_out=build/pyproto \
#         proto/review.proto
#
# 运行(在仓库根目录或 build/ 下都可, --pb2-dir 指到生成的 *_pb2.py 所在目录):
#     python3 scripts/mock_review_server.py --port 50052
#
# 与 config/config.test.yaml 对应的场景演示:
#     --mode auto      (默认) 按 frame_seq 奇偶确定性地"确认/否决", 便于对照 CSV
#     --mode confirm   全部确认  -> 应看到告警写入(复核确认: label=no_helmet ...)
#     --mode reject    全部否决  -> 不应出现任何告警
#     --mode drop      模拟服务不可达(返回 UNAVAILABLE) -> 统计 unavailable=N
#     --mode error     模拟内部错误(返回 INTERNAL, 客户端记为 failed)
#     --delay-ms 2000  模拟慢服务 -> 配合 review.timeout_ms=1000 复现 Timeout 分支
#
# 观察点: 本脚本每次收到请求都会打印一行(证明复核是**异步**的, 由复核
#         worker 线程发出, 不阻塞解码/推理/sink); CVInfer-Gate 退出时会打印
#         "复核统计: submitted=.. reviewed=.. confirmed=.. rejected=.. timeout=.. unavailable=.."
#         告警内容落在 MySQL 的 alerts 表, 库不可用时落到 build/db_fallback.csv。
#
# [T37] 接真 VLM: 本脚本仍是"规则 mock"。要用**真实 VLM** 复核, 请改用
#         vlm_review/ 包(同一份 review.proto, 支持 OpenAI 兼容 / 本地 transformers):
#             python3 -m vlm_review.server --backend openai \
#                 --base-url http://127.0.0.1:8000/v1 --model Qwen/Qwen2.5-VL-7B-Instruct
#         二者可互换, C++ 侧只认 review.endpoint。
# ============================================================
from __future__ import annotations

import argparse
import itertools
import os
import random
import sys
import time
from concurrent import futures


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description="CVInfer-Gate 复核服务 Mock (Phase C)")
    ap.add_argument("--host", default="0.0.0.0", help="监听地址(默认 0.0.0.0)")
    ap.add_argument("--port", type=int, default=50052, help="监听端口(需与 review.endpoint 一致)")
    ap.add_argument("--pb2-dir", default=os.path.join("build", "pyproto"),
                    help="protoc 生成的 *_pb2.py 所在目录")
    ap.add_argument("--mode", default="auto",
                    choices=["auto", "confirm", "reject", "drop", "error"],
                    help="返回策略(见文件头部说明)")
    ap.add_argument("--delay-ms", type=int, default=0, help="每次调用的额外延迟(ms), 用于复现超时")
    ap.add_argument("--seed", type=int, default=20260926, help="随机种子(默认固定 -> 可复现)")
    ap.add_argument("--max-rpcs", type=int, default=0, help="处理满 N 个请求后自动退出(0=不退出)")
    return ap


def main() -> int:
    args = build_parser().parse_args()

    # 1) 让 python 找到 protoc 生成的模块(review_pb2 / review_pb2_grpc)
    here = os.path.dirname(os.path.abspath(__file__))
    for d in (args.pb2_dir,
              os.path.join(here, os.pardir, "build", "pyproto"),
              os.path.join(here, "..", "build", "pyproto")):
        if d and os.path.isdir(d):
            sys.path.insert(0, os.path.abspath(d))

    try:
        import grpc
        import review_pb2 as pb
        import review_pb2_grpc as pb_grpc
    except ImportError as exc:
        print(f"[mock] 导入失败: {exc}", file=sys.stderr)
        print("[mock] 请先执行:", file=sys.stderr)
        print("    pip install grpcio grpcio-tools", file=sys.stderr)
        print("    mkdir -p build/pyproto && python3 -m grpc_tools.protoc -I proto \\", file=sys.stderr)
        print("        --python_out=build/pyproto --grpc_python_out=build/pyproto \\", file=sys.stderr)
        print("        proto/review.proto", file=sys.stderr)
        return 2

    rng = random.Random(args.seed)
    # 序号必须**原子自增**: grpc.server 用线程池并发收请求(C++ 端 review.worker_threads=2),
    # 早期用 state["n"] += 1 会出现两条并发请求都打印 "#2" 的竞态
    # (itertools.count 的 __next__ 是 C 实现, 不会被 GIL 打断)。
    rpc_no = itertools.count(1)
    state = {"n": 0}

    def decide(frame_seq: int) -> bool:
        """auto: 按 frame_seq 奇偶确定性返回, 便于把 CSV 里的告警与帧号对起来"""
        if args.mode == "confirm":
            return True
        if args.mode == "reject":
            return False
        return (int(frame_seq) % 2) == 1

    class ReviewServicer(pb_grpc.ReviewServiceServicer):
        def Review(self, request, context):  # noqa: N802 (protoc 生成的方法名)
            n = next(rpc_no)
            state["n"] = n
            seq = int(request.frame_seq)

            # 延迟: 用于复现客户端的 deadline 超时分支
            if args.delay_ms > 0:
                time.sleep(args.delay_ms / 1000.0)

            # 不可达 / 错误: 用 gRPC 状态码模拟, 客户端会映射成
            # Unavailable / Failed(见 src/review/GrpcLlmReviewer.cpp)
            if args.mode == "drop":
                context.set_code(grpc.StatusCode.UNAVAILABLE)
                context.set_details("mock: 模拟复核服务不可达")
                return pb.ReviewResponse(ok=False)
            if args.mode == "error":
                context.set_code(grpc.StatusCode.INTERNAL)
                context.set_details("mock: 模拟复核服务内部错误")
                return pb.ReviewResponse(ok=False)

            confirmed = decide(seq)
            if confirmed:
                label, conf = "no_helmet", round(rng.uniform(0.80, 0.95), 3)
            else:
                label, conf = "with_helmet", round(rng.uniform(0.90, 0.99), 3)

            # 注意: request.confidence 是**送审时的置信度**。若启用了多模态融合,
            # 它是融合**后**的值(原始视觉值只存在于 DetectionResult::vision_confidence,
            # 未过 proto), 故这里不叫“视觉置信度”以免误读。
            reason = (f"mock[{args.mode}] seq={seq} 标签={request.label} "
                      f"送审置信度={request.confidence:.2f} jpeg={len(request.image_jpeg)}B")

            print(f"[mock] #{n:<4} frame_seq={seq:<6} label={request.label:<8} "
                  f"conf={request.confidence:.2f} -> confirmed={confirmed} "
                  f"({label}, {conf})", flush=True)

            return pb.ReviewResponse(ok=True, confirmed=confirmed,
                                     label=label, confidence=conf, reason=reason)

        # [T37] 健康探测: 仅当 pb2 是重新生成过的(含 HealthResponse)时才暴露;
        #       否则基类会自然返回 UNIMPLEMENTED, 客户端已作兼容(视为可达)。
        if hasattr(pb, "HealthResponse"):
            def Health(self, request, context):  # noqa: N802
                return pb.HealthResponse(ready=True, backend="mock",
                                         model="mock-vlm",
                                         detail=f"mode={args.mode}")

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
    pb_grpc.add_ReviewServiceServicer_to_server(ReviewServicer(), server)
    addr = f"{args.host}:{args.port}"
    server.add_insecure_port(addr)
    server.start()

    print(f"[mock] 复核服务已启动: {addr} (mode={args.mode}, delay={args.delay_ms}ms)")
    print("[mock] 等待 CVInfer-Gate 的复核请求... 按 Ctrl+C 退出")

    try:
        while True:
            time.sleep(1.0)
            if args.max_rpcs > 0 and state["n"] >= args.max_rpcs:
                print(f"[mock] 已处理 {state['n']} 个请求(--max-rpcs), 退出。")
                break
    except KeyboardInterrupt:
        print(f"\n[mock] 收到中断, 共处理 {state['n']} 个请求, 正在停止...")

    server.stop(0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
