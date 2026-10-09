#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""座位配置**文本层**自测（无第三方依赖；不需要 flask/cv2/grpc/模型/相机）。

跑法（仓库根目录，两种都行）：
    python3 web_gateway/test_seats_yaml.py
    python3 -m web_gateway.test_seats_yaml

为什么值得单独测：网页"点选座位 → 回写配置"对商家许了三条承诺，
而三条都是**肉眼看不出、改一行代码就可能悄悄破掉**的：

  1) **只换 `occupancy.seats` 这一个块** —— 其余每一个字节（注释/空行/键顺序/
     行尾注释）原样保留；块内前导注释（写在 `seats:` 与第一个条目之间，通常正是
     "为什么把 zone 画在这儿"的标定依据）必须被带到新块前面。
  2) **读得回来才写得回去** —— 解析（parse_seats）与回写（splice_seats →
     render_seats_yaml）必须对同一批写法都成立：双引号/转义引号/单引号(`''`)/裸标量/
     行尾注释/行内空列表 `seats: []`。
  3) 这里的自检只管"数据本身说不通"（重名/形状/负坐标/越出画面）。
     "两个座位重叠多少算多"**不在这里** —— 那是判定口径, 只由 C++ 的
     ConfigParser::validate() 定义（app.py 调 `CVInfer-Gate --check-config` 判定）。
     若在这个模块里重写一遍, 就会出现"网页说能存、程序却起不来"的分叉。
"""
from __future__ import annotations

import os
import sys

# 支持 `python3 web_gateway/test_seats_yaml.py` 与 `python3 -m web_gateway.test_seats_yaml`
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import seats_yaml as SY  # noqa: E402  (必须在 sys.path 之后)

_FAILED = []


def check(name, ok, detail=""):
    print(("  [OK]   " if ok else "  [FAIL] ") + name + ("" if ok else "  -> " + str(detail)))
    if not ok:
        _FAILED.append(name)


# 现场配置的"仿真样本"：故意带上商家真会写的那些东西（现场说明注释、行尾注释、
# 单/双引号座位名、块尾给下一个键写的注释）。
SAMPLE = """\
# 现场 A 标定记录（2026-08-12 晚, 打烊后重标）
camera:
  source: "rtsp://192.168.1.9:8554/cam1"
  width: 1184
  height: 720

occupancy:
  enabled: true
  dwell_seconds: 30
  seats:
    # 测试视频里 laptop 反复出现在这一带(x 360~720, y 320~960), 且该区域**从未**
    # 检出 person => 正好是"桌面上放着东西、人不在"的工位。
    # ⚠ 不要把 zone 画得太大: 画到"人坐的地方"就会命中判据 C2(人在使用), 永远不判占座。
    - name: "A-12"                                # 靠窗那张
      rect: [360, 320, 360, 640]
    - name: 'B-01''号'
      polygon: [[40, 700], [340, 700], [340, 1240], [40, 1240]]
  # 这条注释是写给下一个键的, 不属于 seats 块
  item_seat_overlap: 0.30

alert:
  enabled: false
"""


def _comments(text):
    return sum(1 for ln in text.splitlines() if ln.lstrip().startswith("#"))


def case_parse_quoting_styles():
    seats, hint = SY.parse_seats(SAMPLE)
    check("解析出 2 个座位（引号写法与行尾注释都不影响）",
          hint is None and len(seats) == 2, (hint, seats))
    if len(seats) != 2:
        return
    a, b = seats
    check("双引号名 + 行尾注释 => 名字不带引号也不带注释", a["name"] == "A-12", a["name"])
    check("rect 保留原始写法记号（回写时商家原来怎么写就怎么给）",
          a["kind"] == "rect" and a["rect"] == [360, 320, 360, 640], a)
    check("rect 展开成 4 个顶点（画图直接用）",
          a["polygon"] == [[360, 320], [720, 320], [720, 960], [360, 960]], a["polygon"])
    check("单引号名里的 '' 还原成一个 '", b["name"] == "B-01'号", b["name"])
    check("polygon 逐点解析为整数",
          b["kind"] == "polygon" and len(b["polygon"]) == 4
          and b["polygon"][0] == [40, 700], b)


def case_parse_escaped_quotes():
    # 回归: 旧正则用 [^"'\n]+? 抓名字, 名字里一出现转义引号就整条匹配失败,
    # 座位被**静默漏掉**（页面少画一个座位, 谁也不会发现）。
    text = ('occupancy:\n  seats:\n'
            '    - name: "桌 \\"A\\": 靠窗"\n      rect: [10, 20, 30, 40]\n'
            '    - name: 前台裸标量\n      rect: [50, 20, 30, 40]\n')
    seats, _ = SY.parse_seats(text)
    check("名字含转义引号 != 消失", [s["name"] for s in seats] == ['桌 "A": 靠窗', "前台裸标量"],
          seats)
    check("裸标量名也能读出来（不需要引号）",
          len(seats) == 2 and seats[1]["rect"] == [50, 20, 30, 40], seats)


def case_empty_and_missing():
    s, hint = SY.parse_seats("occupancy:\n  enabled: true\n  seats: []\n")
    check("`seats: []` => 空列表 + 明确 hint（页面据此提示去补一段 seats）",
          s == [] and hint and "seats" in hint, (s, hint))
    s, hint = SY.parse_seats("occupancy:\n  enabled: true\n")
    check("occupancy 段里没有 seats => 空列表 + hint", s == [] and bool(hint), (s, hint))
    s, hint = SY.parse_seats("camera:\n  width: 10\n")
    check("没有 occupancy 段 => 空列表 + 说明是没开占座",
          s == [] and "occupancy" in hint, (s, hint))


def case_splice_keeps_everything_else():
    seats = [{"name": "C-07", "rect": [100, 200, 300, 400]}]
    new, kept, err = SY.splice_seats(SAMPLE, seats)
    check("替换成功", err is None and bool(new), err)
    if err:
        return
    o_lines, n_lines = SAMPLE.splitlines(keepends=True), new.splitlines(keepends=True)
    o0, o1, _ = SY.seats_block_span(o_lines)
    n0, n1, _ = SY.seats_block_span(n_lines)
    check("seats 块之前的字节逐字不变（文件头注释/相机配置原样）",
          "".join(n_lines[:n0]) == "".join(o_lines[:o0]))
    check("seats 块之后的字节逐字不变（块尾注释 + item_seat_overlap + alert 段）",
          "".join(n_lines[n1:]) == "".join(o_lines[o1:]))
    check("注释行数不变（注释没被吃掉）", _comments(new) == _comments(SAMPLE),
          f"{_comments(SAMPLE)} -> {_comments(new)}")
    check("块内 3 行前导注释被保留（kept 计数回给页面）", kept == 3, kept)
    for ln in o_lines[o0 + 1:o1]:
        if ln.lstrip().startswith("#"):
            check("前导注释原文在位: " + ln.strip()[:26] + "…", ln in new)
    check("块尾那条写给下一个键的注释留在原处（没被吞进 seats 块）",
          "  # 这条注释是写给下一个键的, 不属于 seats 块\n  item_seat_overlap: 0.30\n" in new)
    back, _ = SY.parse_seats(new)
    check("写回去的能被读回来（写→读对称）",
          len(back) == 1 and back[0]["name"] == "C-07"
          and back[0]["rect"] == [100, 200, 300, 400], back)


def case_splice_inline_empty_list():
    text = "occupancy:\n  enabled: true\n  seats: []   # 还没标\n"
    new, kept, err = SY.splice_seats(
        text, [{"name": "A-1", "polygon": [[0, 0], [10, 0], [10, 10]]}])
    check("`seats: []` 上能挂条目", err is None and bool(new), err)
    if err:
        return
    check("行内空列表被还原成块写法，且行尾注释保留",
          "\n  seats:   # 还没标\n" in new, repr(new))
    check("条目按原缩进挂在下面", '\n    - name: "A-1"\n' in new, repr(new))
    check("没有前导注释时 kept=0", kept == 0, kept)
    back, _ = SY.parse_seats(new)
    check("回读得到这个座位", len(back) == 1 and back[0]["name"] == "A-1", back)


def case_splice_missing_block():
    new, kept, err = SY.splice_seats("occupancy:\n  enabled: true\n",
                                     [{"name": "A", "rect": [0, 0, 1, 1]}])
    check("找不到 seats 块 => 明确报错（让用户先手加一段空的）",
          new is None and bool(err) and "seats" in err, err)


def case_validate_geometry():
    v = SY.validate_seats_payload
    check("空列表 => 提示至少要有 1 个座位区",
          any("至少要有一个" in x for x in v([], 100, 100)), v([], 100, 100))
    check("名字必填", any("没写名字" in x for x in v([{"rect": [0, 0, 10, 10]}], 100, 100)))
    check("名字不重名", any("重复" in x for x in v(
        [{"name": "A", "rect": [0, 0, 10, 10]}, {"name": "A", "rect": [50, 50, 10, 10]}],
        100, 100)))
    check("rect 宽/高必须为正",
          any("必须为正" in x for x in v([{"name": "A", "rect": [0, 0, 0, 10]}], 100, 100)))
    check("rect 坐标为负",
          any("为负" in x for x in v([{"name": "A", "rect": [-5, 0, 10, 10]}], 100, 100)))
    check("越出画面（最常见画错：越界的 zone 永远不会有目标落进来）",
          any("超出画面" in x for x in v([{"name": "A", "rect": [90, 90, 30, 30]}], 100, 100)))
    check("rect 必须是 4 个数",
          any("四个数" in x for x in v([{"name": "A", "rect": [1, 2, 3]}], 100, 100)))
    check("polygon 顶点 < 3 => 提示双击闭合",
          any("3 个顶点" in x for x in v([{"name": "A", "polygon": [[0, 0], [1, 1]]}], 100, 100)))
    check("polygon 顶点越界",
          any("超出画面" in x for x in v(
              [{"name": "A", "polygon": [[0, 0], [5, 0], [5, 120]]}], 100, 100)))
    check("既没有 rect 也没有 polygon",
          any("既没有" in x for x in v([{"name": "A"}], 100, 100)))
    check("合法输入 => 一条问题都不报", v([{"name": "A", "rect": [0, 0, 100, 100]}], 100, 100) == [])
    check("拿不到画面尺寸时跳过越界判定（别瞎报）",
          v([{"name": "A", "rect": [9999, 9999, 10, 10]}], 0, 0) == [])
    # ⚠ 这条是"口径单一来源"的边界: 两个座位重叠**不在这里**报错 ——
    #    重叠率是否超标由 C++ ConfigParser::validate() 说了算(web 端会 spawn --check-config)。
    check("重叠**不**由本模块判定（口径只归 C++）",
          v([{"name": "A", "rect": [0, 0, 50, 50]},
             {"name": "B", "rect": [25, 25, 50, 50]}], 200, 200) == [])
    check("整数化：360.0 不会渲染成 360.0（num() 保持 int）",
          SY.num("360") == 360 and isinstance(SY.num("360"), int) and SY.num("360.5") == 360.5)


def case_render_style():
    lines = SY.render_seats_yaml([{"name": '桌 "A"', "rect": [1, 2, 3, 4]},
                                  {"name": "B", "polygon": [[5, 6], [7, 8], [9, 10]]}], 4)
    check("名字一律写成双引号并转义内部引号",
          lines[0] == '    - name: "桌 \\"A\\""\n', repr(lines[0]))
    check("rect 写成整数数组（与 config.example.yaml 一致）",
          lines[1] == "      rect: [1, 2, 3, 4]\n", repr(lines[1]))
    check("polygon 写成 [[x, y], …] 且不带 .0",
          lines[3] == "      polygon: [[5, 6], [7, 8], [9, 10]]\n", repr(lines[3]))
    text = "occupancy:\n  enabled: true\n  seats:\n" + "".join(lines)
    back, _ = SY.parse_seats(text)
    check("渲染出来的写法能被 parse 读回（写法对称，含转义引号名）",
          [s["name"] for s in back] == ['桌 "A"', "B"], back)


def main():
    print("[test_seats_yaml] 座位配置文本层自测（零第三方依赖；不碰真配置、不起进程）")
    for fn in (case_parse_quoting_styles, case_parse_escaped_quotes, case_empty_and_missing,
               case_splice_keeps_everything_else, case_splice_inline_empty_list,
               case_splice_missing_block, case_validate_geometry, case_render_style):
        print(f"- {fn.__name__}")
        fn()
    if _FAILED:
        print(f"\n[FAIL] {len(_FAILED)} 项未通过: {_FAILED}")
        return 1
    print("\n[OK] 全部通过：只换 seats 这一段、读写法对称、判定口径没被复制。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
