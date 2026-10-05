#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""复核服务配置(环境变量驱动, 与 C++ 侧 config.yaml 的 review: 段解耦).

服务端配置全部来自环境变量(前缀 ``VLM_``), 因此同一份镜像可用不同环境变量
指向不同的 VLM(本地 vLLM / Ollama / 云 API), 无需改代码。
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import List


def _env(name: str, default: str) -> str:
    v = os.environ.get(name)
    return v if v not in (None, "") else default


def _env_int(name: str, default: int) -> int:
    v = os.environ.get(name)
    if v in (None, ""):
        return default
    try:
        return int(v)
    except ValueError:
        raise ValueError(f"环境变量 {name}={v!r} 不是合法整数")


def _env_float(name: str, default: float) -> float:
    v = os.environ.get(name)
    if v in (None, ""):
        return default
    try:
        return float(v)
    except ValueError:
        raise ValueError(f"环境变量 {name}={v!r} 不是合法浮点数")


@dataclass
class VlmConfig:
    """复核服务运行配置."""

    # ---- 服务监听 ----
    host: str = "0.0.0.0"
    port: int = 50052
    max_workers: int = 4                 # gRPC 线程池大小(并发复核数)
    pb2_dir: str = ""
    auth_token: str = ""                # [T41] 非空 = 要求调用方带 authorization: Bearer <token>                   # protoc 生成的 *_pb2.py 目录; 空 = <repo>/build/pyproto

    # ---- 后端选择 ----
    #   openai       : 任意 OpenAI 兼容多模态 HTTP 服务(vLLM/Ollama/LM Studio/DashScope/OpenAI)
    #   transformers : 本地 Qwen2.5-VL(需 transformers/torch/qwen-vl-utils)
    #   mock         : 确定性(按 frame_seq 奇偶), 供自检/CI
    backend: str = "openai"
    model: str = ""                      # 空 = 用后端默认模型名

    # ---- openai 后端 ----
    base_url: str = "http://127.0.0.1:8000/v1"
    api_key: str = "EMPTY"
    request_timeout_s: float = 30.0      # 单次上游 HTTP 超时(秒)
    max_tokens: int = 160
    temperature: float = 0.0
    extra_headers: List[str] = field(default_factory=list)  # "K: V" 形式, 逗号分隔

    # ---- transformers 后端 ----
    device: str = "auto"                 # auto | cpu | cuda | cuda:0 ...
    dtype: str = "auto"                  # auto | float16 | bfloat16 | float32
    max_new_tokens: int = 160

    # ---- 业务语义 ----
    # default_prompt: C++ 侧 review.prompt 为空时的兜底任务描述;
    # 留空则用场景自带的 default_task(见 prompts.Scenario)。
    default_prompt: str = ""
    # 业务场景: 决定 system prompt / label 词表 / 关键词兜底表。
    # 可选值见 prompts.scenario_names(); 未知值会**回退默认**(仅告警, 不报错)。
    scenario: str = "helmet"
    # 非空 = 直接覆盖场景自带的 system prompt(用于临时自定义业务, 不推荐的长期做法)
    system_prompt: str = ""

    # ---- 观测 ----
    log_every: int = 1                   # 每 N 个请求打印一行(1 = 全部)

    @classmethod
    def from_env(cls) -> "VlmConfig":
        cfg = cls(
            host=_env("VLM_HOST", cls.host),
            port=_env_int("VLM_PORT", cls.port),
            auth_token=_env("VLM_AUTH_TOKEN", cls.auth_token),
            max_workers=_env_int("VLM_MAX_WORKERS", cls.max_workers),
            backend=_env("VLM_BACKEND", cls.backend).lower(),
            model=_env("VLM_MODEL", cls.model),
            base_url=_env("VLM_BASE_URL", cls.base_url),
            api_key=_env("VLM_API_KEY", cls.api_key),
            request_timeout_s=_env_float("VLM_REQUEST_TIMEOUT_S", cls.request_timeout_s),
            max_tokens=_env_int("VLM_MAX_TOKENS", cls.max_tokens),
            temperature=_env_float("VLM_TEMPERATURE", cls.temperature),
            device=_env("VLM_DEVICE", cls.device),
            dtype=_env("VLM_DTYPE", cls.dtype),
            max_new_tokens=_env_int("VLM_MAX_NEW_TOKENS", cls.max_new_tokens),
            default_prompt=_env("VLM_PROMPT", cls.default_prompt),
            scenario=_env("VLM_SCENARIO", cls.scenario),
            system_prompt=_env("VLM_SYSTEM_PROMPT", cls.system_prompt),
            log_every=_env_int("VLM_LOG_EVERY", cls.log_every),
        )
        headers = os.environ.get("VLM_EXTRA_HEADERS")
        if headers:
            cfg.extra_headers = [h.strip() for h in headers.split(",") if h.strip()]
        return cfg
