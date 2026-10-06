#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""服务端鉴权拦截器自测(无第三方依赖; 不需要 grpcio / pytest, 也不需要相机和模型)。

跑法(仓库根目录):
    python3 -m vlm_review.test_auth_interceptor
    python3 vlm_review/test_auth_interceptor.py

为什么用"假 grpc"而不是真起服务:
  * 本拦截器的正确性属于**纯逻辑**(挑 metadata、定长比较、拒绝时返回同类型
    handler、非 ASCII 不炸), 用一个假 grpc 模块就能完整覆盖;
  * 真正只有"拦到没拦到"这层拼装属于**集成**, 由 README 里
    `vlm_review/server.py` + `./build/review_client` 的端到端命令覆盖
    (本机若没装 grpcio/grpcio-tools 就跑不了那一步, 见 README 说明)。

为什么值得单独测: 鉴权写错的后果是**静默的** —— 要么谁都能调(没拦住),
要么谁都调不通(拦过头), 而真实链路里每次调用只走两条分支, 平时根本看不出来。

注: 本文件不导入 grpc —— `_AuthInterceptor` 能被 import, 正是因为它是鸭子类型
    (不继承 grpc.ServerInterceptor, 原因见该类 docstring)。
"""
from __future__ import annotations

import os
import sys

# 支持 `python3 vlm_review/test_auth_interceptor.py` 直接跑
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vlm_review.server import _AuthInterceptor   # noqa: E402  (必须在 sys.path 之后)


# ---- 假 grpc: 只实现拦截器真正用到的那几个接口 ----
class _FakeHandler:
    def __init__(self, kind, behavior, rd=None, rs=None):
        self._kind = kind
        self.behavior = behavior
        self.request_deserializer = rd
        self.response_serializer = rs

    # 真 grpc 的 handler 用这两个属性表明自己是哪种 RPC
    @property
    def unary_unary(self):
        return self._kind == "unary_unary"

    @property
    def unary_stream(self):
        return self._kind == "unary_stream"


class _FakeGrpc:
    class StatusCode:
        UNAUTHENTICATED = "UNAUTHENTICATED"
        UNKNOWN = "UNKNOWN"

    @staticmethod
    def unary_unary_rpc_method_handler(behavior, request_deserializer=None,
                                       response_serializer=None):
        return _FakeHandler("unary_unary", behavior, request_deserializer,
                            response_serializer)

    @staticmethod
    def unary_stream_rpc_method_handler(behavior, request_deserializer=None,
                                        response_serializer=None):
        return _FakeHandler("unary_stream", behavior, request_deserializer,
                            response_serializer)


class _Aborted(Exception):
    """模拟 context.abort(): 真 grpc 里它也是**抛异常**中断 handler。"""

    def __init__(self, code, details):
        super().__init__(f"{code}: {details}")
        self.code = code
        self.details = details


class _FakeContext:
    def __init__(self):
        self.aborted = None

    def abort(self, code, details=""):
        self.aborted = (code, details)
        raise _Aborted(code, details)


class _CallDetails:
    def __init__(self, metadata):
        self.invocation_metadata = metadata
        self.method = "/review.ReviewService/Review"


_FAILED = []


def check(name, cond, extra=""):
    if cond:
        print(f"  [ok]   {name}")
    else:
        print(f"  [FAIL] {name} {extra}")
        _FAILED.append(name)


def _call(interceptor, metadata, kind="unary_unary"):
    """跑一次 intercept_service, 返回 (原始 handler, 拦截后拿到的 handler)。

    continuation 模拟 grpc 的下一步: 拿走原 handler。业务逻辑用不重复的常量
    值标记, 只要"拿到的不是原对象"就说明被拦了。
    """
    if kind == "unary_unary":
        original = _FakeHandler("unary_unary", lambda req, ctx: "ORIGINAL")
    else:
        original = _FakeHandler("unary_stream", lambda req, ctx: iter(()))

    def continuation(_details):
        return original

    return original, interceptor.intercept_service(continuation, _CallDetails(metadata))


# ---------------- 用例 ----------------
def case_token_ok():
    it = _AuthInterceptor("s3cr3t", _FakeGrpc)
    orig, got = _call(it, (("authorization", "Bearer s3cr3t"),))
    check("正确 token => 放行且**原样返回原 handler**", got is orig)
    check("放行 => 拒绝计数 0", it.denied == 0)


def case_missing_metadata():
    it = _AuthInterceptor("s3cr3t", _FakeGrpc)
    orig, got = _call(it, ())                       # 完全不带 metadata
    check("不带 metadata => 拒绝", got is not None and got is not orig)
    ctx = _FakeContext()
    try:
        got.behavior(None, ctx)
        check("拒绝时 behavior 抛 abort", False, "(竟然没抛)")
    except _Aborted as e:
        check("拒绝码 = UNAUTHENTICATED", e.code == _FakeGrpc.StatusCode.UNAUTHENTICATED)
    check("ctx.abort 被调用", ctx.aborted is not None)
    check("拒绝计数 +1", it.denied == 1)


def case_wrong_token():
    it = _AuthInterceptor("s3cr3t", _FakeGrpc)
    bad_ones = ["Bearer wrong",     # 错 token
                "s3cr3t",           # 漏了 Bearer 前缀
                "Bearer ",          # 空 token
                "Bearer s3cr3t ",   # 尾空格
                "bearer s3cr3t",    # 方案名小写(与 C++ 侧发的 'Bearer ' 精确对齐)
                "Bearer s3cr3tx"]    # 前缀攻击: 多一个字符
    for bad in bad_ones:
        original, got = _call(it, (("authorization", bad),))
        check(f"拒绝 {bad!r}", got is not None and got is not original)
        # 真发生一次调用: 计数在 behavior 被调时 +1(与真 gRPC 一致)
        ctx = _FakeContext()
        try:
            got.behavior(None, ctx)
            check(f"{bad!r} 应 abort", False, "(没抛)")
        except _Aborted:
            pass
    check("6 次调用全计入拒绝计数", it.denied == len(bad_ones))


def case_other_metadata_ignored():
    it = _AuthInterceptor("s3cr3t", _FakeGrpc)
    md = (("user-agent", "grpc-c++/1.60"), ("authorization", "Bearer s3cr3t"),
          ("roi", "roi=240x240"))
    orig, got = _call(it, md)
    check("metadata 里有其它键时仍能挑出 authorization", got is orig)


def case_non_ascii_metadata():
    it = _AuthInterceptor("s3cr3t", _FakeGrpc)
    orig, got = _call(it, (("authorization", "Bearer 中文"),))
    check("非 ASCII => 干净拒绝(不把 TypeError 泄成 UNKNOWN)",
          got is not None and got is not orig)
    ctx = _FakeContext()
    try:
        got.behavior(None, ctx)
        check("非 ASCII => 状态码仍是 UNAUTHENTICATED", False, "(没抛)")
    except _Aborted as e:
        check("非 ASCII => 状态码仍是 UNAUTHENTICATED",
              e.code == _FakeGrpc.StatusCode.UNAUTHENTICATED)


def case_handler_type_preserved():
    it = _AuthInterceptor("s3cr3t", _FakeGrpc)
    _, got_stream = _call(it, (), kind="unary_stream")
    check("unary_stream 原型 => 拒绝 handler 也是 unary_stream", got_stream.unary_stream)
    _, got_unary = _call(it, (), kind="unary_unary")
    check("unary_unary 原型 => 拒绝 handler 也是 unary_unary", got_unary.unary_unary)


def main():
    print("[test_auth_interceptor] 服务端鉴权拦截器自测(无 grpcio 依赖)")
    for fn in (case_token_ok, case_missing_metadata, case_wrong_token,
               case_other_metadata_ignored, case_non_ascii_metadata,
               case_handler_type_preserved):
        print(f"- {fn.__name__}")
        fn()
    if _FAILED:
        print(f"\n[FAIL] {len(_FAILED)} 项未通过: {_FAILED}")
        return 1
    print("\n[OK] 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
