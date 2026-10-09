#!/usr/bin/env python3
"""座位标定回写自检（P1）：`/api/seats` 读 → 校验 → 回写 YAML。

为什么值得单独有个脚本: 网页"点选座位 zone 并回写配置"对商家许了三个很硬的承诺,
而这三个承诺都是**肉眼看不出、改一行代码就可能悄悄破掉**的:

  ① **只动 `occupancy.seats` 这一段** —— 其余每一个字节(注释/空行/键顺序/行尾注释)
     原样保留。配置文件是商家的现场记录, 用 YAML 库 round-trip 会把注释清光。
  ② **判定口径不复制** —— "座位重叠 >10% / 顶点为负 / 重名"只由 C++ 的
     `ConfigParser::validate()` 定义。Python 若自己重写一遍, 就会出现
     "网页说能存、程序却起不来"的分叉。所以这里断言: **重叠必须被 C++ 拦下, 且不写文件**。
  ③ **写回的配置真能被加载** —— 落盘前后都要求 `CVInfer-Gate --check-config` 返回 0。
  ④ **[P2] 即时反馈的契约** —— 拖动时网页会每 ~300ms 用 dry_run 问一次 C++, 于是必须:
     ①干跑**零副作用**(不写盘、不动 mtime、不重建 .bak); ②拒绝理由要**点名到座位**
     (前端据此把那个 zone 描红)。这两条破了页面不会报错 —— 只是悄悄不再提示了,
     正是最该被 CI 抓住的那种退化。

用法（仓库根目录，需 .venv 依赖与已构建的 C++ 程序）：
    python3 scripts/selfcheck_seats_writeback.py
退出码 0 = 全部通过。**全程只操作配置文件的临时副本, 绝不碰仓库里的真配置。**
"""

import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "web_gateway"))
import app as A  # noqa: E402  （导入会自检依赖, 缺了会直接给中文安装说明）
import seats_yaml as SY  # noqa: E402  （零依赖的文本层; 与 app.py 用的是同一份）

FAILS = []


def check(name, ok, detail=""):
    print(("  [OK]   " if ok else "  [FAIL] ") + name + ("" if ok else "  -> " + str(detail)))
    if not ok:
        FAILS.append(name)


def main():
    src = A.CONFIG_PATH
    if not src or not os.path.isfile(src):
        print(f"找不到配置（CVINFER_CONFIG={src}）—— 先准备一份 config/*.yaml")
        return 2
    orig = open(src, encoding="utf-8").read()
    work = tempfile.mkdtemp(prefix="seats_selfcheck_")
    cand = os.path.join(work, "config.yaml")
    shutil.copy2(src, cand)
    A.CONFIG_PATH = cand                     # ← 只在副本上动手
    c = A.app.test_client()

    print("1) 读配置里的座位")
    got = c.get("/api/seats")
    d = got.get_json() or {}
    check("GET /api/seats 返回 200", got.status_code == 200, got.status_code)
    check("能解析出座位（或明确给出 hint）", bool(d.get("seats")) or bool(d.get("hint")),
          d)

    base = [{"name": "A-12", "rect": [360, 320, 360, 640]}]

    print("2) 只校验不写入（dry_run，也该被 C++ 验一遍）")
    r = c.post("/api/seats", json={"seats": base, "width": 720, "height": 1280,
                                   "dry_run": True})
    dd = r.get_json() or {}
    check("dry_run 通过且标注口径来自 C++",
          r.status_code == 200 and dd.get("checked_by") == "c++", dd.get("issues") or dd)
    check("dry_run 不写文件", open(cand, encoding="utf-8").read() == orig)

    print("3) 判定口径必须由 C++ 拦（两端都在画面内的重叠 zone）")
    r = c.post("/api/seats", json={"seats": [
        {"name": "A-12", "rect": [360, 320, 360, 640]},
        {"name": "A-13", "rect": [500, 500, 200, 200]}],
        "width": 720, "height": 1280})
    dd = r.get_json() or {}
    msg = " ".join(dd.get("issues") or [])
    check("重叠被拒（400）", r.status_code == 400, r.status_code)
    check("拒绝理由是 C++ 的重叠校验原文", "重叠" in msg and dd.get("checked_by") == "c++", msg)
    check("校验失败时原文件一字未改", open(cand, encoding="utf-8").read() == orig)

    print("4) 越出画面由结构自检拦（不必等 C++）")
    r = c.post("/api/seats", json={"seats": [{"name": "A-12", "rect": [600, 320, 360, 640]}],
                                   "width": 720, "height": 1280, "dry_run": True})
    check("越界被拒并点名越界", r.status_code == 400 and
          "超出画面" in " ".join((r.get_json() or {}).get("issues") or []),
          (r.get_json() or {}).get("issues"))

    print("5) 真写：1 个 rect + 1 个 polygon")
    r = c.post("/api/seats", json={"seats": base + [
        {"name": "B-01", "polygon": [[40, 700], [340, 700], [340, 1240], [40, 1240]]}],
        "width": 720, "height": 1280})
    dd = r.get_json() or {}
    check("写回成功（2 个座位）", r.status_code == 200 and dd.get("count") == 2, dd)
    new = open(cand, encoding="utf-8").read()

    print("6) 承诺①：只有 seats 这一段变了, 其余字节逐字保留")
    o_lines = orig.splitlines(keepends=True)
    o_start, o_end, _ = SY.seats_block_span(o_lines) or (None, None, None)
    check("原文能定位到 occupancy.seats 段", o_start is not None)
    if o_start is not None:
        n_lines = new.splitlines(keepends=True)
        n_start, n_end, _ = SY.seats_block_span(n_lines) or (None, None, None)
        check("原 seats 块之前的字节逐字不变",
              "".join(n_lines[:n_start]) == "".join(o_lines[:o_start]))
        check("原 seats 块之后的字节逐字不变",
              "".join(n_lines[n_end:]) == "".join(o_lines[o_end:]))
        check("注释行数不变（注释没被吃掉）",
              sum(l.lstrip().startswith("#") for l in o_lines)
              == sum(l.lstrip().startswith("#") for l in n_lines),
              f"原={sum(l.lstrip().startswith('#') for l in o_lines)} "
              f"新={sum(l.lstrip().startswith('#') for l in n_lines)}")
        lead = [l for l in o_lines[o_start + 1:o_end] if l.lstrip().startswith("#")]
        check("原块内前导注释被带到新块前", all(l in new for l in lead), lead)
    bak = cand + A.CONFIG_BACKUP_SUFFIX
    check("生成了 .bak 且与原文一致",
          os.path.isfile(bak) and open(bak, encoding="utf-8").read() == orig)

    print("7) 承诺③：写回的配置能被 C++ 加载, 且能被原样读回来")
    if A.CVINFER_BIN and os.path.isfile(A.CVINFER_BIN):
        p = subprocess.run([A.CVINFER_BIN, "--check-config", "--config", cand],
                           capture_output=True, text=True)
        check("CVInfer-Gate --check-config 退出码 0", p.returncode == 0,
              (p.stderr or "").strip()[-200:])
    else:
        print("  [SKIP] 没找到 C++ 程序（CVINFER_BIN）—— 跳过加载校验")
    back = c.get("/api/seats").get_json() or {}
    seats = back.get("seats") or []
    check("座位数与几何读回来一致",
          [s["name"] for s in seats] == ["A-12", "B-01"]
          and seats[0].get("rect") == [360, 320, 360, 640]
          and seats[1]["polygon"] == [[40, 700], [340, 700], [340, 1240], [40, 1240]],
          seats)

    # ---- [P2] 即时反馈契约: 拖动中每 ~300ms 一次 dry_run ⇒ 必须零副作用 + 能点名到座位 ----
    print("8) [P2] 即时反馈的契约（干跑零副作用 + 拒绝理由点名到座位）")
    txt_before = open(cand, encoding="utf-8").read()
    st_before = os.stat(cand)
    baks = cand + A.CONFIG_BACKUP_SUFFIX
    st_bak_before = os.stat(baks) if os.path.isfile(baks) else None
    for _ in range(3):                     # 模拟"拖着不放、被反复问"
        c.post("/api/seats", json={"seats": base, "width": 720, "height": 1280,
                                   "dry_run": True})
    check("反复 dry_run 不改配置内容", open(cand, encoding="utf-8").read() == txt_before)
    check("反复 dry_run 不动 mtime（即时反馈不许碰配置）",
          os.stat(cand).st_mtime_ns == st_before.st_mtime_ns)
    if st_bak_before is not None:
        check("反复 dry_run 不重做 .bak（备份只在真写时做）",
              os.stat(baks).st_mtime_ns == st_bak_before.st_mtime_ns)

    def dry_issues(seats):
        rr = c.post("/api/seats", json={"seats": seats, "width": 720, "height": 1280,
                                        "dry_run": True})
        return rr.status_code, " ".join((rr.get_json() or {}).get("issues") or [])

    code, msg = dry_issues([{"name": "A-12", "rect": [360, 320, 360, 640]},
                            {"name": "A-13", "rect": [500, 500, 200, 200]}])
    check("重叠：dry_run 同样拒（400）", code == 400, code)
    check("重叠文案点名两个座位（前端据此把 zone 描红）",
          "[A-12]" in msg and "[A-13]" in msg, msg)

    code, msg = dry_issues([{"name": "A-12", "rect": [600, 320, 360, 640]}])
    check("越界文案点名座位且说清越界（本地提示的口径与后端同源）",
          code == 400 and "[A-12]" in msg and "超出画面" in msg, msg)

    code, msg = dry_issues([{"name": "A-12", "rect": [40, 40, 100, 100]},
                            {"name": "A-12", "rect": [400, 400, 100, 100]}])
    check("重名文案带「座位名重复：<名字>」（前端认这个前缀）",
          code == 400 and "座位名重复：A-12" in msg, msg)

    code, msg = dry_issues([{"rect": [40, 40, 100, 100]}])
    check("未命名座位被称作「第 1 个座位」（前端认这个称呼）",
          code == 400 and "第 1 个座位" in msg, msg)

    code, msg = dry_issues(base)
    check("干净的一份：dry_run 通过（前端显示绿字）", code == 200, (code, msg))

    # 前端的两个即时行是**按 id 挂钩**的: 改模板/改 JS 时把 id 打错, 表现是"页面照旧打开、
    # 只是再也不提示了" —— 没有异常、没有红字, 最难发现。这里用一次 GET 把它钉住。
    page = c.get("/").get_data(as_text=True)
    check("页面里两个即时行元素都在（editChk / editTip）",
          'id="editChk"' in page and 'id="editTip"' in page)
    js = c.get("/static/app.js")
    check("静态 app.js 能取到, 且带着 P2 的即时校验入口",
          js.status_code == 200 and b"edCheckSoon" in js.data
          and b"checked_note" in js.data)

    # 提示行（id=editHint）被几处代码写: 探针跑完的说明 / 读入配置 / 保存结果。实测踩过的坑:
    # JS 里写成 edHint（少个 it）—— 语法合法、括号也配平, 静态查不出; 运行时 ReferenceError,
    # 而且它发生在"配置已经写盘成功"**之后** ⇒ 商家看到"请求失败", 文件其实已经改了。
    # 同类的还有把 editHeat 写成 edHeat。这里把"手柄名与模板 id 对得上"钉成契约
    # （scripts/jscheck.py 会再对全量名字查一遍）。
    check("提示行手柄名与模板 id 对得上（editHint, 不许写成 edHint）",
          'id="editHint"' in page and b'editHint = $("editHint")' in js.data
          and b"edHint." not in js.data and b"edHeat." not in js.data)

    # 本地提示必须自报口径: 计数是**整段视频**抽样出来的、底图只是**首帧**。实测踩过:
    # 商家画在只有物品的桌面上, 却看到"人的落点多于物品" —— 因为首帧没人、视频里有人。
    # 后端得把 stride 一起回给前端, 前端得在"整段视频一个物品标签都没抓到"时改口。
    app_src = open(A.__file__, encoding="utf-8").read()
    check("探针把 stride 回给前端（提示要自报\"每 N 帧抽 1 帧\"）",
          "stride=SEAT_PROBE_STRIDE" in app_src and b"d.stride" in js.data)
    check("提示自报口径: 整段视频 / 首帧 / 抓不到物品时不怪 zone",
          "整段视频" in js.data.decode("utf-8")
          and "首帧" in js.data.decode("utf-8")
          and "没有抓到任何物品标签" in js.data.decode("utf-8")
          and b"label_top" in js.data)

    # "C++ 程序不在" 时后端只能放行 —— 但必须**如实**报成"跳过校验"(checked_by=skipped),
    # 页面靠这个字段把绿字换成橙色告警。若这里退回一句绿字"校验通过", 就是拿没校验过的
    # 结果骗商家 —— 而 P2 的全部说服力都建立在"红/绿字就是 C++ 的判决"上。
    bin_saved = A.CVINFER_BIN
    try:
        A.CVINFER_BIN = os.path.join(work, "no-such-cvinfer")     # _check_config 读的是这个全局
        rr = c.post("/api/seats", json={"seats": base, "width": 720, "height": 1280,
                                        "dry_run": True})
        jj = rr.get_json() or {}
        check("缺 C++ 程序时如实回 checked_by=skipped + 告警原文（前端改打橙色）",
              jj.get("ok") is True and jj.get("checked_by") == "skipped"
              and bool(jj.get("checked_note")), jj)
    finally:
        A.CVINFER_BIN = bin_saved

    print("9) 「为什么一条告警都没有」的自证（P3）")
    # 实测最贵的一次白干: 商家画好座位、跑完视频, 页面只说「本轮未检测到占座」——
    # 真相是**一帧都没处理**（视频源打不开）。下面这段合成日志就是那次的日志, 后端必须
    # 一眼认出来, 而且每条结论都要能指回**日志原文**（不许页面自己编结论）。
    logs_dir = os.path.join(work, "logs")
    os.makedirs(logs_dir, exist_ok=True)
    synlog = os.path.join(logs_dir, "cvinfer.log")
    with open(synlog, "w", encoding="utf-8") as f:
        f.write(
            "2026-10-09 19:23:44.100 [INFO ] [1] === CVInfer-Gate 启动 ===\n"
            "2026-10-09 19:23:44.700 [WARN ] [1] 视频源打开失败, 跳过本地视频流水线, "
            "但 gRPC 微服务仍会启动!\n"
            "2026-10-09 19:23:44.795 [INFO ] [1] 服务就绪, 按 Ctrl+C 退出。\n")
    save_dir, save_log = A.RESULT_DIR, A.RESULT_LOG
    try:
        A.RESULT_DIR, A.RESULT_LOG = work, synlog
        jj = c.get("/api/events").get_json() or {}
        dg = jj.get("diagnosis") or {}
        vs = dg.get("verdicts") or []
        flat = " ".join((x.get("title") or "") + " " + (x.get("detail") or "") for x in vs)
        check("/api/events 带 diagnosis（页面才有东西可自证）", bool(vs), list(jj.keys()))
        check("认出「一帧都没处理」并点明要在 build/ 下启动",
              any(x.get("level") == "err" and "一帧都没处理" in (x.get("title") or "")
                  for x in vs) and "build/" in flat, flat[:200])
        check("每条结论都带日志原文（可核对, 不许页面自己编）",
              all(x.get("evidence") for x in vs), [x for x in vs if not x.get("evidence")])
        cfg = dg.get("config") or {}
        check("回显「网页在编辑哪份配置」+ 它有没有 occupancy 段",
              bool(cfg.get("path")) and "has_occupancy" in cfg and "item_labels" in cfg, cfg)
    finally:
        A.RESULT_DIR, A.RESULT_LOG = save_dir, save_log

    keep_log = A.RESULT_LOG
    try:
        A.RESULT_LOG = os.path.join(work, "no-such-log.log")     # 连日志都没找到时
        dg2 = (c.get("/api/events").get_json() or {}).get("diagnosis") or {}
        v2 = dg2.get("verdicts") or []
        check("没有日志时也自证, 并说清网页在读哪条路径",
              bool(v2) and all(x.get("evidence") for x in v2), v2)
    finally:
        A.RESULT_LOG = keep_log

    check("模板里有自证面板 #resDiag", 'id="resDiag"' in page)
    check("前端手柄名与模板 id 对得上（resDiag, 不许写成 edDiag/rdiag 之类）",
          b'resDiag = $("resDiag")' in js.data)
    check("前端把 diagnosis 呈现出来（含日志原文折叠）",
          b"renderDiag" in js.data and b"s.diagnosis" in js.data and b"diag-ev" in js.data)
    check("标定提示能区分「没识别到」与「认出来了但没算成物品」（引用 item_labels）",
          b"edItemLabels" in js.data
          and "不在 occupancy.item_labels" in js.data.decode("utf-8"))
    check("标定提示能说清「物品在人身上 ⇒ 多半随身, 规则层会排除」",
          "随身" in js.data.decode("utf-8")
          and "物品在人身上" in js.data.decode("utf-8"))
    check("探针把 item_labels 一起回给前端（且前端真的读了）",
          "item_labels=" in app_src and b"d.item_labels" in js.data)
    check("探针回显它要写回的配置路径（改哪份文件不再是猜）", b"d.config" in js.data)

    print("-" * 60)
    if FAILS:
        print(f"FAILED（{len(FAILS)} 项）: " + "; ".join(FAILS))
        return 1
    print("全部通过：seats 回写只动那一段、口径由 C++ 判、写回的配置能被加载。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
