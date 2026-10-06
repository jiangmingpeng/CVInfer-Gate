#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""提示词构建 + 结论解析(把 VLM 的自由文本收敛为结构化判定).

职责边界:
  * ``Scenario``: 一个**业务场景**的完整语义包 —— system prompt、正/负标签、
    默认任务、关键词兜底表。换业务(占座 / 其他业务 / ...)只需要换一个 Scenario,
    解析逻辑一行都不用动。
  * ``build_system_prompt`` / ``build_user_prompt``: 面向业务的指令, 要求 VLM
    输出**严格 JSON**, 便于稳定解析。
  * ``parse_verdict``: 对 VLM 输出做**多级容错解析**(JSON -> 关键词 -> 兜底),
    这是"接真 VLM"最容易翻车的一环(不同模型/版本输出格式漂移), 故单列。
  * ``Verdict``: 结构化结论; ``confirmed=True`` 表示"确认异常, 应告警"。

为什么要有 Scenario(踩过的坑):
  system prompt 与 label 词表**不能写死成某一个业务**, 而送往模型的 task 文本
  来自 C++ 侧 config 的 ``review.prompt``。两者描述的不是同一件事时, 三者立刻互相打架:
      system = "<历史业务的 system prompt / label 词表>"
      user   = "任务: 判断该座位是否被长期占座"
      image  = 一本书的 107x135 特写
  模型给不出任何合理答案, 只能编一个形如 ``long_time_use`` 的假 label, 且
  ``confirmed`` 恒为 false => 复核永不告警("看起来接了 VLM, 其实一句都对不上")。
  结论: 场景必须能跟着业务走, 并且能被 config / 环境变量覆盖。
        本仓库当前业务 = 图书馆/自习室**占座**(``seat_occupancy``), 即默认场景。
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Dict, Optional, Tuple


@dataclass(frozen=True)
class Scenario:
    """一个业务场景的完整语义包."""

    name: str                          # 场景标识(VLM_SCENARIO / --scenario)
    description: str                   # 一句话说明(启动日志用)
    system_prompt: str                 # system 角色指令(必须要求严格 JSON)
    positive_label: str                # "确认异常 / 应告警" 的 label
    negative_label: str                # "否决异常 / 不告警" 的 label
    default_task: str                  # review.prompt 为空时的 user 侧任务描述
    positive_phrases: Tuple[str, ...]  # 关键词兜底: 判"应告警"的短语
    negative_phrases: Tuple[str, ...]  # 关键词兜底: 判"不告警"的短语

    @property
    def known_labels(self) -> Tuple[str, ...]:
        """词表内合法的 label; 词表外的都算脏值, 会被归一化。"""
        return (self.positive_label, self.negative_label)


# ------------------------------------------------------------------
# 场景: 图书馆/自习室 占座(本仓库当前业务 = 默认场景)
# ------------------------------------------------------------------
# 关键: system 里必须**明确"看不到人就倾向占座"与"看到人就不是占座"**,
#       并给出"信息不足 => confirmed=false"的退出路径 —— 否则模型会对着
#       一张书本特写硬猜一个理由出来。
SEAT_OCCUPANCY_SCENARIO = Scenario(
    name="seat_occupancy",
    description="图书馆/自习室占座: 座位上有物品但看不到人在使用",
    positive_label="occupied",
    negative_label="not_occupied",
    default_task="判断该座位区域是否被占座(桌面/座位上有物品, 但看不到正在使用该座位的人)",
    system_prompt=(
        "你是图书馆/自习室的座位管理视觉复核助手。你会收到一张从监控画面"
        "裁剪出的区域图, 图中通常包含桌面、座椅以及桌面/座椅上的物品"
        "(书、包、笔记本电脑等)。\n"
        "请判断该座位区域是否处于【占座】状态 —— 即桌面上有物品, 但画面里"
        "看不到正在使用该座位的人。\n"
        "判断要点(务必逐条核对):\n"
        "  1) 画面里能看到有人坐在/站在该座位旁并使用桌面物品 => not_occupied;\n"
        "  2) 只有物品、看不到人 => occupied;\n"
        "  3) 画面信息不足(例如只有一本书的特写, 看不到座位与人) => "
        "confirmed=false, 不要凭空猜测。\n"
        "只依据图像内容判断, 不要臆测。"
        "必须只输出一个 JSON 对象, 不要输出任何解释或 Markdown 代码块, 格式如下:\n"
        '{"confirmed": true|false, "label": "occupied|not_occupied", '
        '"confidence": 0.0-1.0, "reason": "不超过40字的简要依据"}'
    ),
    positive_phrases=(
        "occupied", "long_time_use", "long-term use", "long term use",
        "long_time_occupied", "reserving", "reserved", "hogging", "seat hogging",
        "占座", "长期占用", "长时间占用", "长期使用", "长时间使用",
        "物品占位", "占位", "无人使用", "没有人使用", "无人看管",
    ),
    negative_phrases=(
        "not_occupied", "not occupied", "unoccupied", "vacant", "empty seat",
        "in use", "in_use", "person present", "someone is sitting", "reading",
        "未占座", "不是占座", "没有占座", "无人占座", "有人使用", "有人在",
        "有人坐", "正在使用", "短暂离开", "正常使用", "未占用",
    ),
)

SCENARIOS: Dict[str, Scenario] = {
    SEAT_OCCUPANCY_SCENARIO.name: SEAT_OCCUPANCY_SCENARIO,
    # 别名: 便于命令行 / 环境变量随手写
    "seat": SEAT_OCCUPANCY_SCENARIO,
    "occupancy": SEAT_OCCUPANCY_SCENARIO,
}

# 默认场景 = 本仓库当前业务(图书馆/自习室占座)。
# 新增业务: 再定义一个 Scenario 并登记进 SCENARIOS 即可, 解析逻辑无需改动。
DEFAULT_SCENARIO = SEAT_OCCUPANCY_SCENARIO

# 向后兼容: 旧的模块级常量(= 默认场景的取值), 供既有 import 继续使用
POSITIVE_LABEL = DEFAULT_SCENARIO.positive_label
NEGATIVE_LABEL = DEFAULT_SCENARIO.negative_label
SYSTEM_PROMPT = DEFAULT_SCENARIO.system_prompt


def scenario_names() -> Tuple[str, ...]:
    """去重后的场景名(用于日志 / 报错提示)。"""
    return tuple(sorted({s.name for s in SCENARIOS.values()}))


def resolve_scenario(name: Optional[str]) -> Scenario:
    """按名字取场景; 未知名字回退默认场景(调用方可用 scenario_names() 提示)。"""
    if not name:
        return DEFAULT_SCENARIO
    return SCENARIOS.get(str(name).strip().lower(), DEFAULT_SCENARIO)


def build_system_prompt(scenario: Optional[Scenario] = None, override: str = "") -> str:
    """取 system prompt: 显式 override(非空)优先于场景自带。"""
    if override and override.strip():
        return override.strip()
    return (scenario or DEFAULT_SCENARIO).system_prompt


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


def build_user_prompt(task_prompt: str, label: str, confidence: float,
                      scenario: Optional[Scenario] = None) -> str:
    """把业务提示 + 主模型证据拼成 user 文本.

    注意: task 文本(来自 C++ 侧 review.prompt)只描述**任务**, 它**不能**改变
    system 角色的场景定义 —— 所以 system 必须由同一个 Scenario 供给, 否则就会
    出现 "system 说 A 业务、user 说 B 业务" 的打架(见模块 docstring)。
    """
    task = (task_prompt or "").strip() or (scenario or DEFAULT_SCENARIO).default_task
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


def _is_negated(low: str, start: int) -> bool:
    """判断 low[start:] 处的短语在当前文本里是否**紧跟否定词**(被否定了)。

    为什么需要它: 短语表匹配是**纯子串**的, 而否定词会把后面短语的意思整个翻过来:
        "该座位未占座" 里含正表 '占座';
        "not occupied" / "unoccupied" 里都含正表 'occupied'。
    若不做否定翻转, 这些"否定结论"会被正表抢先命中, 判成"占座"(方向性错误 => 误告警)。
    所以先认出"被否定的命中", 再把它**翻转**为相反极性。
    """
    if start <= 0:
        return False
    # 中文否定词: 紧邻一个字就够(未/沒/没/不/无/無/非)
    if low[start - 1] in "未沒没不无無非":
        return True
    # 英文否定词: 向前剥离分隔符(空格/_/-)后, 看是否以否定词结尾且处于词首
    i = start
    while i > 0 and low[i - 1] in " _-":
        i -= 1
    head = low[:i]
    for w in ("without", "not", "non", "no", "un", "n't"):
        if head.endswith(w):
            j = i - len(w)
            if j == 0 or not low[j - 1].isalnum():
                return True
    return False


def _by_keywords(text: str, scenario: Optional[Scenario] = None) -> Optional[Tuple[bool, str]]:
    """关键词兜底解析: **最长命中**决定归属, 命中若被否定则**翻转极性**。

    两条规则合起来才能兼顾中英两类真实文本:
      * 英文: 'not occupied' / 'not_occupied' / 'unoccupied' 里都含着正表 'occupied',
              靠"否定翻转"把它们纠正为 not_occupied(而不是误报占座);
      * 中文: '未占座' / '没有占座' 里也含着正表 '占座',
              同样靠"否定翻转"扭回 not_occupied(而不是误报占座)。
    """
    sc = scenario or DEFAULT_SCENARIO
    low = (text or "").lower()
    best_len = 0
    best: Optional[Tuple[bool, str]] = None

    def consider(phrase: str, positive_table: bool) -> None:
        nonlocal best_len, best
        idx = low.find(phrase)
        if idx < 0 or len(phrase) <= best_len:
            return
        # 正表短语被否定 => 实际是"否定结论"; 负表短语被否定 => 实际是"肯定结论"。
        negated = _is_negated(low, idx)
        confirmed = (not negated) if positive_table else negated
        best_len = len(phrase)
        best = (confirmed, sc.positive_label if confirmed else sc.negative_label)

    for p in sc.positive_phrases:
        consider(p, True)
    for n in sc.negative_phrases:
        consider(n, False)
    return best


def _normalize_label(raw_label: str, confirmed: bool, scenario: Scenario) -> str:
    """把模型给的 label 收敛到场景词表内。

    模型(尤其小模型)常给出词表外的 label —— 被互相打架的 prompt 搞懵时会输出
    'long_time_use' 之类。旧实现只对 {"", "person", "unknown"} 兜底, 于是脏
    label 一路透传到 DB / 告警。现在: **label 必须与 confirmed 同极性** ——
    关键词只用来在**同极性内**挑更贴切的取值, 不允许反过来推翻 confirmed
    (confirmed 是契约里的"是否告警"布尔, 它才是准绳; 否则会出现
    "confirmed=false 但 label=occupied" 这种自相矛盾的记录)。
    """
    lab = (raw_label or "").strip().lower()
    if lab in scenario.known_labels:
        return lab
    kw = _by_keywords(lab, scenario)
    if kw is not None and kw[0] == confirmed:
        return kw[1]
    return scenario.positive_label if confirmed else scenario.negative_label


def parse_verdict(text: str, scenario: Optional[Scenario] = None) -> Verdict:
    """把 VLM 输出解析为 Verdict; 无法解析时兜底为"不确认"(宁可不告警).

    分级:
      1) JSON 解析(首选): 直接读 confirmed/label/confidence/reason;
      2) 关键词解析: 命中场景的正/负词表(中英双语, 最长命中优先);
      3) 兜底: confirmed=False, confidence=0.0, reason 标注解析失败, 便于排查。

    归一化: 词表外的 label(模型自由发挥)会被收敛到场景的正/负 label,
            避免 'long_time_use' 这类脏值写进 DB 与告警描述。
    """
    sc = scenario or DEFAULT_SCENARIO
    text = (text or "").strip()
    if not text:
        return Verdict(False, sc.negative_label, 0.0, "空输出")

    obj = _extract_json(text)
    if obj is not None:
        raw_label = str(obj.get("label", "")).strip().lower()
        confirmed = obj.get("confirmed")
        if not isinstance(confirmed, bool):
            # 没有显式 bool 时, 用 label 关键词兜底
            kw = _by_keywords(raw_label, sc)
            confirmed = kw[0] if kw else False
        label = _normalize_label(raw_label, bool(confirmed), sc)
        return Verdict(bool(confirmed), label,
                       _clamp_conf(obj.get("confidence", 0.0)),
                       str(obj.get("reason", "")).strip()[:120])

    kw = _by_keywords(text, sc)
    if kw is not None:
        confirmed, label = kw
        return Verdict(confirmed, label,
                       0.75 if confirmed else 0.85,
                       "关键词解析: " + text[:60].replace("\n", " "))

    return Verdict(False, sc.negative_label, 0.0,
                   "解析失败(既非 JSON 也无关键词): " + text[:60].replace("\n", " "))
