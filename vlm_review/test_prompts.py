#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""场景(system prompt / label 词表)与结论解析自测。

无第三方依赖(不需要 grpcio / pytest / 模型), 只 import 纯逻辑模块。

跑法(仓库根目录):
    python3 -m vlm_review.test_prompts
    python3 vlm_review/test_prompts.py

为什么值得单独测 —— 这里每一个用例都对应一个**真实踩过的坑**:
  1) 场景没跟着业务走: C++ 侧 review.prompt 写"占座", 服务端 system 还停在
     "安全帽", 模型只能编一个 long_time_use 的假 label 且 confirmed 恒为 false
     => "看起来接了 VLM, 其实一句都对不上"。故断言 seat 场景的 system 里必须
     出现"占座"与 occupied|not_occupied。
  2) 关键词包含关系: 'occupied' 是 'not occupied'/'unoccupied' 的子串, 旧实现
     "先判正再判负"会把"不占座"误判成"占座"(方向性错误, 直接导致误告警)。
     故断言 longest-match 后 'not occupied' 判 not_occupied。
  3) 脏 label 透传: 词表外的 label 会一路写进 DB / 告警描述, 污染数据。
     故断言 'long_time_use' 被归一化到 occupied / not_occupied。
"""
from __future__ import annotations

import os
import sys

# 支持 `python3 vlm_review/test_prompts.py` 直接跑
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from vlm_review.prompts import (HELMET_SCENARIO, SEAT_OCCUPANCY_SCENARIO,  # noqa: E402
                                build_system_prompt, build_user_prompt,
                                parse_verdict, resolve_scenario, scenario_names)

_FAILED = []


def check(name, cond, extra=""):
    if cond:
        print(f"  [ok]   {name}")
    else:
        print(f"  [FAIL] {name} {extra}")
        _FAILED.append(name)


# ---------------- 场景解析 ----------------
def case_resolve_scenario():
    check("缺省 -> helmet", resolve_scenario("").name == "helmet")
    check("None -> helmet", resolve_scenario(None).name == "helmet")
    check("seat_occupancy", resolve_scenario("seat_occupancy").name == "seat_occupancy")
    check("别名 seat", resolve_scenario("seat").name == "seat_occupancy")
    check("大小写/空格容错", resolve_scenario("  SEAT_OCCUPANCY  ").name == "seat_occupancy")
    # 未知场景回退默认(故意不抛异常: 环境变量拼错不该让旁路服务起不来)
    check("未知 -> 回退 helmet", resolve_scenario("no_such_scenario").name == "helmet")
    names = scenario_names()
    check("scenario_names 含两个业务场景",
          "helmet" in names and "seat_occupancy" in names, f"got={names}")


# ---------------- system / user prompt ----------------
def case_prompts():
    seat_sp = build_system_prompt(SEAT_OCCUPANCY_SCENARIO)
    check("seat 的 system 提到占座", "占座" in seat_sp)
    check("seat 的 system 约束 label 取值", "occupied|not_occupied" in seat_sp)
    check("seat 的 system 要求严格 JSON", "JSON" in seat_sp)
    check("seat 的 system 给出'信息不足=>不确认'的退出路径", "confirmed=false" in seat_sp)

    helmet_sp = build_system_prompt(HELMET_SCENARIO)
    check("helmet 的 system 仍是安全帽语义", "安全帽" in helmet_sp)
    check("helmet 的 system 约束 label 取值", "no_helmet|with_helmet" in helmet_sp)

    # override 优先
    check("override 覆盖场景",
          build_system_prompt(SEAT_OCCUPANCY_SCENARIO, "只输出 OK") == "只输出 OK")
    check("空 override 视为不覆盖",
          build_system_prompt(SEAT_OCCUPANCY_SCENARIO, "   ") == seat_sp)

    # user 任务文本: config 给了就用 config 的; 没给就用场景默认
    up = build_user_prompt("", "book", 0.42, SEAT_OCCUPANCY_SCENARIO)
    check("空 task -> 用场景 default_task", SEAT_OCCUPANCY_SCENARIO.default_task in up)
    check("user 文本带上游证据", "book" in up and "0.42" in up)
    up2 = build_user_prompt("自定义任务", "book", 0.42, SEAT_OCCUPANCY_SCENARIO)
    check("自定义 task 优先", "自定义任务" in up2)


# ---------------- 解析: JSON 路径 ----------------
def case_parse_json():
    v = parse_verdict('{"confirmed": true, "label": "occupied", "confidence": 0.91, "reason": "无人"}',
                      SEAT_OCCUPANCY_SCENARIO)
    check("seat 正例", v.confirmed and v.label == "occupied", f"got={v}")

    v = parse_verdict('{"confirmed": false, "label": "not_occupied", "confidence": 0.8}',
                      SEAT_OCCUPANCY_SCENARIO)
    check("seat 负例", (not v.confirmed) and v.label == "not_occupied", f"got={v}")

    # 脏 label(词表外)按 confirmed 归一化 —— 这是"编 label"那个坑的回归
    v = parse_verdict('{"confirmed": true, "label": "long_time_use", "confidence": 0.5}',
                      SEAT_OCCUPANCY_SCENARIO)
    check("脏 label + confirmed => 归一到 occupied", v.label == "occupied", f"got={v}")
    v = parse_verdict('{"confirmed": false, "label": "long_time_use", "confidence": 0.5}',
                      SEAT_OCCUPANCY_SCENARIO)
    check("脏 label + 未确认 => 归一到 not_occupied", v.label == "not_occupied", f"got={v}")

    # 没有显式 bool 时, 用 label 关键词兜底
    v = parse_verdict('{"label": "not occupied", "confidence": 0.7}', SEAT_OCCUPANCY_SCENARIO)
    check("缺 confirmed => label 关键词兜底(否定)", (not v.confirmed) and v.label == "not_occupied",
          f"got={v}")

    # 安全帽场景: 与改造前行为一致
    v = parse_verdict('{"confirmed": true, "label": "no_helmet", "confidence": 0.9}', HELMET_SCENARIO)
    check("helmet 正例不变", v.confirmed and v.label == "no_helmet", f"got={v}")
    v = parse_verdict('{"confirmed": true, "label": "未戴安全帽"}', HELMET_SCENARIO)
    check("helmet 中文脏 label 归一", v.label == "no_helmet", f"got={v}")


# ---------------- 解析: 关键词路径(包含关系陷阱) ----------------
def case_parse_keywords():
    # 最关键的一条: 'occupied' 是 'not occupied' 的子串
    for text in ("not occupied", "not_occupied", "unoccupied", "The seat is vacant, unoccupied"):
        v = parse_verdict(text, SEAT_OCCUPANCY_SCENARIO)
        check(f"否定短语不被 'occupied' 抢先命中: {text!r}",
              (not v.confirmed) and v.label == "not_occupied", f"got={v}")

    for text in ("occupied", "该座位长期占用，属于占座", "无人使用，物品占位"):
        v = parse_verdict(text, SEAT_OCCUPANCY_SCENARIO)
        check(f"肯定短语命中: {text!r}", v.confirmed and v.label == "occupied", f"got={v}")

    # 安全帽场景: '未佩戴'(3) 比 '佩戴'(2) 长 => 仍判"未佩戴"
    v = parse_verdict("该人员未佩戴安全帽", HELMET_SCENARIO)
    check("helmet 中文最长命中(未佩戴)", v.confirmed and v.label == "no_helmet", f"got={v}")
    v = parse_verdict("该人员已佩戴安全帽", HELMET_SCENARIO)
    check("helmet 中文否定(已佩戴)", (not v.confirmed) and v.label == "with_helmet", f"got={v}")


# ---------------- 解析: 兜底 ----------------
def case_parse_fallback():
    v = parse_verdict("", SEAT_OCCUPANCY_SCENARIO)
    check("空输出 => 不确认", (not v.confirmed) and v.label == "not_occupied", f"got={v}")
    v = parse_verdict("模型今天不想说话", SEAT_OCCUPANCY_SCENARIO)
    check("无法解析 => 不确认(宁可不告警)",
          (not v.confirmed) and v.label == "not_occupied", f"got={v}")
    check("无法解析 => reason 标注便于排查", "解析失败" in v.reason, f"got={v}")

    # 默认场景 = helmet: 不传 scenario 时行为必须与改造前一致
    v = parse_verdict("未佩戴")
    check("默认场景 = helmet(向后兼容)", v.confirmed and v.label == "no_helmet", f"got={v}")


def main():
    print("[test_prompts] 场景(system prompt / label 词表)与结论解析自测(无第三方依赖)")
    for fn in (case_resolve_scenario, case_prompts, case_parse_json,
               case_parse_keywords, case_parse_fallback):
        print(f"- {fn.__name__}")
        fn()
    if _FAILED:
        print(f"\n[FAIL] {len(_FAILED)} 项未通过: {_FAILED}")
        return 1
    print("\n[OK] 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
