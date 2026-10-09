# -*- coding: utf-8 -*-
"""`occupancy.seats` 的**文本层**读写（只依赖标准库, 不 import flask/cv2/grpc）。

为什么单拎成一个模块:
  * 这一层是"每个现场标一次座位"的核心承诺 —— **只换 seats 这一个块, 其余字节
    (注释/空行/键顺序/行尾注释) 一字不动**。配置文件是商家手写的现场记录, 里面
    全是"为什么把 zone 画在这儿"的注释; 用 YAML 库 round-trip 会把这些注释清光,
    等于毁掉他的标定依据。这种"看不出错、改一行就悄悄破掉"的约定必须能被 CI 守住,
    所以它不能埋在 Flask 路由里(= 想测就得装一整套 web 依赖)。
  * 于是: 纯文本处理在这里, HTTP/环境变量/进程调用留在 app.py。
    对应测试 web_gateway/test_seats_yaml.py 零第三方依赖, 直接进 CI:
        python3 -m web_gateway.test_seats_yaml

**不做的事（有意）**: 不重写任何"判定口径"。"座位重叠多少算多 / 顶点能否为负"
只由 C++ 的 ConfigParser::validate() 定义, 由 app.py 调 `CVInfer-Gate --check-config`
判定。本模块只做**结构/几何**自检（名字必填不重名、形状成对、夹在画面内）——
那属于"这份数据本身说不说得通", 与判定规则无关。
"""
from __future__ import annotations

import re

# 只解析 occupancy.seats 里的 name + rect/polygon：够用且**不引入 yaml 依赖**
# （项目口径是"不轻易加依赖"）。解析失败就返回空列表，页面退化为"只显示事件文本"。
SEAT_RE = re.compile(
    # name 支持 YAML 的三种标量写法: 双引号(含 \" \\ 转义) / 单引号('' 表示 ') / 裸标量;
    # 后面允许一个行尾注释。旧写法用 [^"'\n]+? 抓名字 —— 名字里一旦出现转义引号
    # (如 桌 "A": 靠窗 ⇒ YAML 写成 "桌 \"A\": 靠窗")就整条匹配失败, 座位会被静默漏掉。
    r"-\s*name\s*:\s*(?P<name>"
    r'(?:"(?:[^"\\]|\\.)*"|\'(?:[^\']|\'\')*\'|[^\n#]+?)'
    r")\s*(?:#.*)?\n"
    r"\s*(?P<kind>rect|polygon)\s*:\s*(?P<val>[^\n]+)"
)

# 探针 JSON 里的建议 zone / 前端上报的坐标都是**整数像素**
SEAT_NAME_MAX = 64


def unquote_yaml(s):
    """YAML 标量 → 原始字符串（只处理座位名会用到的几种写法）。"""
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        body, out, i = s[1:-1], [], 0
        while i < len(body):
            if body[i] == "\\" and i + 1 < len(body):
                nxt = body[i + 1]
                out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(nxt, nxt))
                i += 2
            else:
                out.append(body[i])
                i += 1
        return "".join(out)
    if len(s) >= 2 and s[0] == "'" and s[-1] == "'":
        return s[1:-1].replace("''", "'")
    return s


def num(txt):
    """YAML 里写 360 就回 360(int)，写 360.5 才回 float —— 免得前端满屏 360.0。"""
    v = float(txt)
    return int(v) if v == int(v) else v


def yaml_str(v):
    """把座位名安全地写成 YAML 双引号字符串（名字里可能有引号/冒号/中文）。"""
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def parse_seats(text):
    """从配置**文本**里读出座位区。返回 (seats, hint)。

    seats = [{"name":…, "polygon":[[x,y],…], "kind":"rect"|"polygon",
              "rect":[x,y,w,h]?}]  —— polygon 一律展开成顶点，画图直接可用；
    kind/rect 只是把**原始写法**留个记号，P1 的"点选标定"回填时要保持商家原来怎么写的。
    解析不出来时 seats 为空并给出 hint（页面据此提示"去补一段 seats"而不是白屏）。
    """
    # 只取 occupancy: 段（到下一个顶格 key 为止），免得扫到别处的 rect/polygon
    m = re.search(r"^occupancy\s*:\s*$", text, re.M)
    if not m:
        return [], "配置里没有 occupancy 段（占座判定未启用？）"
    seg = text[m.end():]
    nxt = re.search(r"^\S", seg, re.M)     # 下一个顶格键 = 本段结束
    if nxt:
        seg = seg[:nxt.start()]

    seats = []
    for sm in SEAT_RE.finditer(seg):
        name = unquote_yaml(sm.group("name"))
        val = sm.group("val").split("#")[0]  # 去掉行尾注释
        nums = [num(v) for v in re.findall(r"-?\d+(?:\.\d+)?", val)]
        if sm.group("kind") == "rect" and len(nums) >= 4:
            x, y, w, h = nums[:4]
            poly = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]
            seats.append({"name": name, "polygon": poly, "kind": "rect",
                          "rect": [x, y, w, h]})
            continue
        elif sm.group("kind") == "polygon" and len(nums) >= 6 and len(nums) % 2 == 0:
            poly = [[nums[i], nums[i + 1]] for i in range(0, len(nums), 2)]
        else:
            continue
        seats.append({"name": name, "polygon": poly, "kind": "polygon"})
    if not seats:
        return [], "配置里没解析出座位（occupancy.seats 为空？）"
    return seats, None


def seats_block_span(lines):
    """在 occupancy: 段里定位 seats: 块的 [start, end) 与条目缩进。找不到返回 None。

    纯文本层定位（不解析 YAML），这样才能做到"只换这一段"。
    - start = `seats:` 这一行的下标
    - end   = 块结束（不含）；块尾紧跟的**空行/注释行**留在块外(通常是给下个键写的注释)
    - item_indent = 列表项 `- name:` 的缩进；沿用原文件写法
    """
    occ_i = None
    for i, ln in enumerate(lines):
        if re.match(r"^occupancy\s*:\s*(#.*)?$", ln):
            occ_i = i
            break
    if occ_i is None:
        return None

    seats_i = None
    for i in range(occ_i + 1, len(lines)):
        ln = lines[i]
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        if len(ln) - len(ln.lstrip()) == 0:
            break                                  # 出了 occupancy 段
        if re.match(r"^\s*seats\s*:\s*(\[\s*\]\s*)?(#.*)?$", ln):
            seats_i = i
            break
    if seats_i is None:
        return None

    key_indent = len(lines[seats_i]) - len(lines[seats_i].lstrip())
    end = len(lines)
    for j in range(seats_i + 1, len(lines)):
        ln = lines[j]
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        ind = len(ln) - len(ln.lstrip())
        # 列表项即使与 key 同缩进（顶格写法）也算本块
        if ind > key_indent or (ind == key_indent and re.match(r"^\s*-\s", ln)):
            continue
        end = j
        break
    while end - 1 > seats_i and (not lines[end - 1].strip()
                                 or lines[end - 1].lstrip().startswith("#")):
        end -= 1                                   # 把块尾的注释留给下一个键
    item_indent = key_indent + 2
    for j in range(seats_i + 1, end):
        m = re.match(r"^(\s*)-\s", lines[j])
        if m:
            item_indent = len(m.group(1))
            break
    return seats_i, end, item_indent


def render_seats_yaml(seats, item_indent):
    """把座位列表渲染成 YAML 行（与 config.example.yaml 的写法保持一致）。"""
    pad = " " * item_indent
    cont = " " * (item_indent + 2)
    out = []
    for s in seats:
        out.append(pad + "- name: " + yaml_str(s["name"]) + "\n")
        if s.get("rect"):
            x, y, w, h = s["rect"]
            out.append(cont + "rect: [" + ", ".join(str(int(v)) for v in (x, y, w, h)) + "]\n")
        else:
            pts = ", ".join("[" + str(int(p[0])) + ", " + str(int(p[1])) + "]"
                            for p in s["polygon"])
            out.append(cont + "polygon: [" + pts + "]\n")
    return out


def splice_seats(text, seats):
    """把 occupancy.seats 换成 seats，返回 (new_text, kept_comments, err)。

    只换这一个块，其余字节不动。块内**前导注释**（写在 `seats:` 与第一个 `- name:`
    之间的说明，通常正是"为什么把 zone 画在这儿"的标定依据）**保留**并原样带到新块前面
    —— 直接删掉等于销毁商家的现场记录。条目之间/行尾的注释属于被替换掉的那些座位，随之更新。
    """
    lines = text.splitlines(keepends=True)
    span = seats_block_span(lines)
    if span is None:
        return None, 0, ("配置里找不到 occupancy.seats 段 —— 请先手动加一段空的 "
                         "`seats:`（缩进 2 空格，放在 occupancy: 下面），再回来点选。")
    start, end, item_indent = span
    # `seats: []`（行内空列表）必须先还原成块写法 —— 否则"空列表 + 缩进条目"是非法
    # YAML（`seats: []` 后面再挂 `- name:` 会让 yaml-cpp 直接报错）。行尾注释保留。
    key_line = re.sub(r"(\s*seats\s*:)\s*\[\s*\]", r"\1", lines[start])
    lead = []
    for j in range(start + 1, end):
        if re.match(r"^\s*-\s", lines[j]):
            break                                  # 到第一个条目为止
        lead.append(lines[j])
    new_lines = (lines[:start] + [key_line] + lead
                 + render_seats_yaml(seats, item_indent) + lines[end:])
    kept = len([l for l in lead if l.lstrip().startswith("#")])
    return "".join(new_lines), kept, None


def norm_seat(s):
    """规整成 {name, rect:[int×4]} 或 {name, polygon:[[int,int],…]}（全整数）。"""
    name = str(s.get("name") or "").strip()
    if s.get("rect"):
        return {"name": name, "rect": [int(round(float(v))) for v in s["rect"][:4]]}
    return {"name": name,
            "polygon": [[int(round(float(p[0]))), int(round(float(p[1])))]
                        for p in s["polygon"]]}


def validate_seats_payload(seats, width=0, height=0):
    """**结构/几何**层面的自检（不复制任何判定口径）。

    只查"这份数据本身说不通"的东西：名字必填/不重名、rect 或 polygon 形状是否成对、
    坐标为负、以及**越出画面**（越界的 zone 永远不会有目标落进来 —— 最常见的画错）。
    至于"两个座位重叠多少算多"，那是判定口径，交给 C++（见 app.py 的 _check_config）。

    width/height = 画面尺寸；给 0 表示"拿不到画面尺寸" ⇒ 跳过越界判定（不瞎报）。
    """
    if not isinstance(seats, list) or not seats:
        return ["至少要有一个座位区：occupancy.enabled=true 时不能没有 seats"
                "（检测器没有\"座位\"这个概念，座位只能靠配置画出来）"]
    issues, names = [], set()
    for i, s in enumerate(seats):
        if not isinstance(s, dict):
            issues.append(f"第 {i + 1} 个座位的数据格式不对")
            continue
        name = str(s.get("name") or "").strip()
        who = name or f"第 {i + 1} 个座位"
        if not name:
            issues.append(f"第 {i + 1} 个座位没写名字（名字会出现在日志/告警里，"
                          "必须能对上现场的那张桌子）")
        elif len(name) > SEAT_NAME_MAX:
            issues.append(f"[{who}] 名字太长（>{SEAT_NAME_MAX} 字）")
        elif name in names:
            issues.append(f"座位名重复：{name}（重名会让日志/告警/去重互相串味）")
        else:
            names.add(name)

        if s.get("rect"):
            r = s["rect"]
            if len(r) != 4:
                issues.append(f"[{who}] rect 必须是 [x, y, w, h] 四个数")
                continue
            x, y, w, h = (int(round(float(v))) for v in r)
            if w <= 0 or h <= 0:
                issues.append(f"[{who}] rect 的宽/高必须为正（w={w}, h={h}）")
            if x < 0 or y < 0:
                issues.append(f"[{who}] rect 坐标为负（x={x}, y={y}）")
            if width and height and (x + w > width or y + h > height):
                issues.append(f"[{who}] 超出画面 {width}×{height}：右下角到 "
                              f"({x + w}, {y + h})（越界的 zone 永远不会有目标落进来）")
        elif s.get("polygon"):
            pts = s["polygon"]
            if len(pts) < 3:
                issues.append(f"[{who}] polygon 至少要 3 个顶点（多点几次再双击闭合）")
                continue
            for p in pts:
                if len(p) != 2:
                    issues.append(f"[{who}] polygon 顶点应写成 [x, y]")
                    break
                px, py = int(round(float(p[0]))), int(round(float(p[1])))
                if px < 0 or py < 0:
                    issues.append(f"[{who}] polygon 顶点坐标为负：({px}, {py})")
                    break
                if width and height and (px > width or py > height):
                    issues.append(f"[{who}] polygon 顶点超出画面 {width}×{height}："
                                  f"({px}, {py})")
                    break
        else:
            issues.append(f"[{who}] 既没有 rect 也没有 polygon")
    return issues
