#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""提示词构建 + 结论解析(把 VLM 的自由文本收敛为结构化判定).

职责边界:
  * ``build_system_prompt`` / ``build_user_prompt``: 面向业务(安全帽)的指令,
    要求 VLM 输出**严格 JSON**, 便于稳定解析。
  * ``parse_verdict``: 对 VLM 输出做**多级容错解析**(JSON -> 关键词 -> 兜底),
    这是"接真 VLM"最容易翻车的一环(不同模型/版本输出格式漂移), 故单列。
  * ``Verdict``: 结构化结论; ``confirmed=True`` 表示"确认异常, 应告警"。
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Optional, Tuple

# 业务标签约定(与项目 config 的 alert_type / 复核标签保持一致)
POSITIVE_LABEL = "no_helmet"      # 确认异常: 未佩戴 -> 告警
NEGATIVE_LABEL = "with_helmet"    # 否决异常: 已佩戴 -> 不告警

# 关键词表: **顺序敏感**(先判"未佩戴"再判"佩戴", 否则 "未佩戴" 会被 "佩戴" 误命中)
_POSITIVE_PHRASES = (
    "no_helmet", "no helmet", "without helmet", "not wearing", "no-hardhat",
    "未佩戴", "沒有佩戴", "没有佩戴", "没戴", "未戴", "无安全帽", "无头盔",
    "缺少安全帽", "未戴安全帽", "没戴安全帽", "不符合",
)
_NEGATIVE_PHRASES = (
    "with_helmet", "with helmet", "wearing", "has helmet", "compliant",
    "已佩戴", "佩戴了", "佩戴安全帽", "有安全帽", "戴了", "戴有", "符合",
)

SYSTEM_PROMPT = (
    "你是一个工地安全合规视觉复核助手。你会收到一张从监控画面裁剪出的"
    "人员小图。请判断该人员**是否未佩戴安全帽**。"
    "只依据图像内容判断, 不要臆测。"
    "必须只输出一个 JSON 对象, 不要输出任何解释或 Markdown 代码块, 格式如下:\n"
    '{"confirmed": true|false, "label": "no_helmet|with_helmet", '
    '"confidence": 0.0-1.0, "reason": "不超过40字的简要依据"}'
)


@dataclass
class Verdict:
    """结构化复核结论(confirmed=True => 应告警)."""

    confirmed: bool
    label: str
    confidence: float
    reason: str

    def to_dict(self) -> dict:
        return {
            "confirmed": self.confirmed,
            "label": self.label,
            "confidence": self.confidence,
            "reason": self.reason,
        }


def build_user_prompt(task_prompt: str, label: str, confidence: float) -> str:
    """把业务提示 + 主模型证据拼成 user 文本."""
    task = (task_prompt or "").strip() or "判断该人员是否未佩戴安全帽"
    return (
        f"任务: {task}\n"
        f"上游检测器给出的类别: {label or 'unknown'}, 置信度: {confidence:.2f}\n"
        "请给出你的复核结论(严格 JSON)。"
    )


def _clamp_conf(value) -> float:
    try:
        c = float(value)
    except (TypeError, ValueError):
        return 0.0
    if c > 1.0:            # 兼容模型返回百分数(如 90)
        c = c / 100.0
    return max(0.0, min(1.0, c))


def _extract_json(text: str) -> Optional[dict]:
    """从自由文本里抠出第一个 JSON 对象(容忍 ```json 代码块与前后噪声)."""
    # 去掉 markdown 代码围栏
    fenced = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL)
    candidates = []
    if fenced:
        candidates.append(fenced.group(1))
    first = text.find("{")
    last = text.rfind("}")
    if first != -1 and last > first:
        candidates.append(text[first:last + 1])
    for cand in candidates:
        try:
            obj = json.loads(cand)
            if isinstance(obj, dict):
                return obj
        except json.JSONDecodeError:
            continue
    return None


def _by_keywords(text: str) -> Optional[Tuple[bool, str]]:
    low = text.lower()
    for p in _POSITIVE_PHRASES:
        if p in low:
            return True, POSITIVE_LABEL
    for n in _NEGATIVE_PHRASES:
        if n in low:
            return False, NEGATIVE_LABEL
    return None


def parse_verdict(text: str) -> Verdict:
    """把 VLM 输出解析为 Verdict; 无法解析时兜底为"不确认"(宁可不告警).

    分级:
      1) JSON 解析(首选): 直接读 confirmed/label/confidence/reason;
      2) 关键词解析: 命中"未佩戴/已佩戴"词表(中英双语);
      3) 兜底: confirmed=False, confidence=0.0, reason 标注解析失败, 便于排查。
    """
    text = (text or "").strip()
    if not text:
        return Verdict(False, NEGATIVE_LABEL, 0.0, "空输出")

    obj = _extract_json(text)
    if obj is not None:
        raw_label = str(obj.get("label", "")).strip().lower()
        confirmed = obj.get("confirmed")
        if not isinstance(confirmed, bool):
            # 没有显式 bool 时, 用 label 关键词兜底
            kw = _by_keywords(raw_label)
            confirmed = kw[0] if kw else False
        label = raw_label or (POSITIVE_LABEL if confirmed else NEGATIVE_LABEL)
        # 归一化标签: 让上游/DB 拿到稳定取值
        if confirmed and label in ("", "person", "unknown"):
            label = POSITIVE_LABEL
        if not confirmed and label in ("", "person", "unknown"):
            label = NEGATIVE_LABEL
        return Verdict(bool(confirmed), label,
                       _clamp_conf(obj.get("confidence", 0.0)),
                       str(obj.get("reason", "")).strip()[:120])

    kw = _by_keywords(text)
    if kw is not None:
        confirmed, label = kw
        return Verdict(confirmed, label,
                       0.75 if confirmed else 0.85,
                       "关键词解析: " + text[:60].replace("\n", " "))

    return Verdict(False, NEGATIVE_LABEL, 0.0,
                   "解析失败(既非 JSON 也无关键词): " + text[:60].replace("\n", " "))
