#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CVInfer-Gate 复核服务(Phase C) —— 真 VLM 接入包.

本包实现 ``proto/review.proto`` 的 ``review.ReviewService``(服务端),
把 C++ 网关送来的 ROI 小图交给一个**真实 VLM** 复核, 并按契约返回
``confirmed / label / confidence / reason``。

复核的"业务语义"不再写死: 由 ``prompts.Scenario`` 描述(安全帽 / 占座 / ...),
可用环境变量 ``VLM_SCENARIO`` 或 ``VLM_SYSTEM_PROMPT`` 覆盖。

设计要点:
  * **契约不变**: 复用既有 gRPC 契约, 流水线(C++ 侧)零改动。
  * **后端可插拔**: openai(任意 OpenAI 兼容多模态 HTTP 服务) /
    transformers(本地 Qwen2.5-VL) / mock(确定性, 供自检与 CI)。
  * **场景可配置**: system prompt 与 label 词表跟着业务走(见 ``prompts``)。
  * **无第三方 web 依赖**: 只用标准库 urllib 调上游, 唯一硬依赖是 grpcio。
"""

from .config import VlmConfig                                   # noqa: F401
from .prompts import (Scenario, Verdict, parse_verdict,         # noqa: F401
                      resolve_scenario, scenario_names)
from .backends import build_backend                             # noqa: F401

__all__ = ["VlmConfig", "Verdict", "parse_verdict", "Scenario",
           "resolve_scenario", "scenario_names", "build_backend"]
__version__ = "1.1.0"
