#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""复核后端(可插拔): 把"用哪个 VLM"与"gRPC 服务怎么跑"解耦.

三种后端:
  * OpenAiCompatBackend : 调任意 **OpenAI 兼容**多模态 HTTP 接口。
        现实中最通用的"接真 VLM"方式 —— vLLM / Ollama / LM Studio /
        DashScope(百炼兼容模式) / OpenAI 自身 都暴露该接口, 换模型只改
        环境变量, 不动代码。
  * TransformersBackend : 本地跑 Qwen2.5-VL(需 torch/transformers/
        qwen-vl-utils)。适合无外网 / 隐私敏感场景, 但资源占用大。
  * MockBackend         : 确定性后端(按 frame_seq 奇偶), 供自检/CI, 行为
        与旧 mock 保持一致(既有 CSV/告警断言不受影响)。

所有后端统一返回 ``prompts.Verdict``; 上游错误抛 ``BackendError``,
由 server 层映射为合适的 gRPC 状态码。
"""
from __future__ import annotations

import base64
import json
import threading
import urllib.error
import urllib.request
from abc import ABC, abstractmethod
from typing import Dict, List, Optional, Tuple

from .config import VlmConfig
from .prompts import (NEGATIVE_LABEL, POSITIVE_LABEL, SYSTEM_PROMPT, Verdict,
                      build_user_prompt, parse_verdict)


class BackendError(RuntimeError):
    """后端调用失败。``unavailable=True`` 表示"上游不可达"(可重试/应降级)。"""

    def __init__(self, message: str, *, unavailable: bool = False):
        super().__init__(message)
        self.unavailable = unavailable


class VlmBackend(ABC):
    """复核后端接口。"""

    kind: str = "base"

    @property
    def model_name(self) -> str:
        return self.kind

    def ready(self) -> Tuple[bool, str]:
        """就绪性(仅用于 Health RPC, 不做网络探测以免阻塞启动)."""
        return True, self.kind

    @abstractmethod
    def infer(self, jpeg: bytes, label: str, confidence: float,
              prompt: str, frame_seq: int) -> Verdict:
        """对一张 ROI JPEG 做复核, 返回结构化结论。失败抛 BackendError。"""
        raise NotImplementedError


# ------------------------------------------------------------------
# OpenAI 兼容后端
# ------------------------------------------------------------------
def _parse_extra_headers(items: List[str]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for it in items or []:
        if ":" in it:
            k, v = it.split(":", 1)
            out[k.strip()] = v.strip()
    return out


def _extract_content(obj: dict) -> str:
    """从 OpenAI 兼容响应中取文本(兼容 content 为 str 或分段 list 两种形态)."""
    try:
        content = obj["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as e:
        raise BackendError(f"上游响应结构异常: {e}") from e
    if isinstance(content, str):
        return content
    if isinstance(content, list):   # 多段: [{"type":"text","text":...}, ...]
        parts = [p.get("text", "") for p in content if isinstance(p, dict)]
        return "".join(parts)
    return str(content)


class OpenAiCompatBackend(VlmBackend):
    kind = "openai"

    def __init__(self, cfg: VlmConfig):
        self.base_url = cfg.base_url.rstrip("/")
        self.api_key = cfg.api_key or "EMPTY"
        self.model = cfg.model or "Qwen/Qwen2.5-VL-7B-Instruct"
        self.timeout_s = cfg.request_timeout_s
        self.max_tokens = cfg.max_tokens
        self.temperature = cfg.temperature
        self.extra_headers = _parse_extra_headers(cfg.extra_headers)

    @property
    def model_name(self) -> str:
        return self.model

    def ready(self) -> Tuple[bool, str]:
        return True, f"openai-compatible @ {self.base_url} (model={self.model})"

    def infer(self, jpeg: bytes, label: str, confidence: float,
              prompt: str, frame_seq: int) -> Verdict:
        b64 = base64.b64encode(jpeg).decode("ascii")
        payload = {
            "model": self.model,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": [
                    {"type": "image_url",
                     "image_url": {"url": f"data:image/jpeg;base64,{b64}"}},
                    {"type": "text",
                     "text": build_user_prompt(prompt, label, confidence)},
                ]},
            ],
            "max_tokens": self.max_tokens,
            "temperature": self.temperature,
        }
        headers = {"Content-Type": "application/json",
                   "Authorization": f"Bearer {self.api_key}"}
        headers.update(self.extra_headers)

        req = urllib.request.Request(
            f"{self.base_url}/chat/completions",
            data=json.dumps(payload).encode("utf-8"),
            headers=headers, method="POST")

        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s) as resp:
                body = resp.read()
        except urllib.error.HTTPError as e:
            detail = b""
            try:
                detail = e.read()[:300]
            except Exception:  # noqa: BLE001
                pass
            raise BackendError(
                f"上游 HTTP {e.code}: {detail!r}",
                unavailable=e.code in (429, 502, 503, 504)) from e
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise BackendError(f"上游不可达: {e}", unavailable=True) from e

        try:
            obj = json.loads(body)
        except json.JSONDecodeError as e:
            raise BackendError(f"上游返回非 JSON: {body[:200]!r}") from e
        return parse_verdict(_extract_content(obj))


# ------------------------------------------------------------------
# 本地 transformers 后端 (Qwen2.5-VL / Qwen2-VL)
# ------------------------------------------------------------------
class TransformersBackend(VlmBackend):
    kind = "transformers"

    def __init__(self, cfg: VlmConfig):
        self.model_id = cfg.model or "Qwen/Qwen2.5-VL-7B-Instruct"
        self.device = cfg.device
        self.dtype = cfg.dtype
        self.max_new_tokens = cfg.max_new_tokens
        self._model = None
        self._processor = None
        self._resolved_device = None
        self._lock = threading.Lock()

    @property
    def model_name(self) -> str:
        return self.model_id

    def ready(self) -> Tuple[bool, str]:
        try:
            import torch  # noqa: F401
            from transformers import AutoProcessor  # noqa: F401
        except ImportError as e:
            return False, f"缺少依赖(torch/transformers): {e}"
        return True, f"transformers @ {self.device} (model={self.model_id})"

    def _load(self):
        with self._lock:
            if self._model is not None:
                return
            try:
                import torch
                from transformers import AutoProcessor
            except ImportError as e:
                raise BackendError(f"缺少依赖(torch/transformers): {e}",
                                   unavailable=True) from e
            try:
                from transformers import Qwen2_5_VLForConditionalGeneration as ModelCls
            except ImportError:
                try:
                    from transformers import Qwen2VLForConditionalGeneration as ModelCls
                except ImportError as e:
                    raise BackendError(
                        "transformers 版本过旧, 找不到 Qwen2.5-VL/Qwen2-VL 模型类",
                        unavailable=True) from e

            self._resolved_device = (self.device if self.device != "auto"
                                     else ("cuda" if torch.cuda.is_available() else "cpu"))
            dtype = {"auto": "auto", "float16": torch.float16,
                     "bfloat16": torch.bfloat16, "float32": torch.float32}.get(
                         self.dtype, "auto")
            self._model = ModelCls.from_pretrained(
                self.model_id, torch_dtype=dtype).to(self._resolved_device).eval()
            self._processor = AutoProcessor.from_pretrained(self.model_id)

    def infer(self, jpeg: bytes, label: str, confidence: float,
              prompt: str, frame_seq: int) -> Verdict:
        self._load()
        import io

        import torch
        from PIL import Image
        from qwen_vl_utils import process_vision_info

        image = Image.open(io.BytesIO(jpeg)).convert("RGB")
        messages = [
            {"role": "system", "content": [{"type": "text", "text": SYSTEM_PROMPT}]},
            {"role": "user", "content": [
                {"type": "image", "image": image},
                {"type": "text", "text": build_user_prompt(prompt, label, confidence)},
            ]},
        ]
        text = self._processor.apply_chat_template(
            messages, tokenize=False, add_generation_prompt=True)
        image_inputs, video_inputs = process_vision_info(messages)
        inputs = self._processor(text=[text], images=image_inputs, videos=video_inputs,
                                 padding=True, return_tensors="pt").to(self._resolved_device)
        with torch.no_grad():
            generated = self._model.generate(
                **inputs, max_new_tokens=self.max_new_tokens, do_sample=False)
        trimmed = generated[:, inputs.input_ids.shape[1]:]
        out = self._processor.batch_decode(
            trimmed, skip_special_tokens=True,
            clean_up_tokenization_spaces=False)[0]
        return parse_verdict(out)


# ------------------------------------------------------------------
# Mock 后端 (确定性; 供自检/CI, 行为与旧 mock 一致)
# ------------------------------------------------------------------
class MockBackend(VlmBackend):
    kind = "mock"

    def __init__(self, cfg: Optional[VlmConfig] = None):
        self.model = (cfg.model if cfg and cfg.model else "mock-vlm")

    @property
    def model_name(self) -> str:
        return self.model

    def ready(self) -> Tuple[bool, str]:
        return True, "deterministic mock (frame_seq 奇偶)"

    def infer(self, jpeg: bytes, label: str, confidence: float,
              prompt: str, frame_seq: int) -> Verdict:
        confirmed = (int(frame_seq) % 2) == 1
        if confirmed:
            return Verdict(True, POSITIVE_LABEL, 0.90,
                           f"mock: seq={frame_seq} label={label} -> 未佩戴")
        return Verdict(False, NEGATIVE_LABEL, 0.95,
                       f"mock: seq={frame_seq} label={label} -> 已佩戴")


# ------------------------------------------------------------------
# 工厂
# ------------------------------------------------------------------
def build_backend(cfg: VlmConfig) -> VlmBackend:
    kind = (cfg.backend or "openai").lower()
    if kind == "openai":
        return OpenAiCompatBackend(cfg)
    if kind == "transformers":
        return TransformersBackend(cfg)
    if kind == "mock":
        return MockBackend(cfg)
    raise ValueError(
        f"未知的 VLM_BACKEND={cfg.backend!r} (应为 openai/transformers/mock)")
