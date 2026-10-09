#!/usr/bin/env python3
"""前端静态自检（零依赖, node 不可用时的兜底）：
  ① 括号/引号/注释是否配平；
  ② 有没有"用了但没声明"的标识符（node 不可用 ⇒ 没有 eslint/真语法检查）。

为什么它在仓库里: `web_gateway/static/app.js` 是一个上千行的手写文件, 而环境里没有
node(也就跑不了 eslint/真语法检查)。改这个文件最常见的低级错误有两类:
  · **少一个右括号 / 引号没闭合** —— 表现是"整页 JS 全不执行、控制台一行红字", 但页面
    看起来还在(Flask 照样渲染模板) ⇒ 很容易漏到"商家打开一看啥都不动"。
  · **手柄名拼错**（`edHint` vs `editHint`、`edHeat` vs `editHeat`）—— 语法完全合法, 括号也
    配平, 所以 ① 查不出来；但运行时是 ReferenceError, 而且**往往发生在一个已经成功之后的
    分支**（例如"配置已经写盘了, 只是紧接着更新提示文字时炸了"）⇒ 商家看到"请求失败",
    其实文件已经改了, 反而可能重复点、重复写。这两处拼写错误都是实测踩到的。
所以这里做**两遍粗略扫描**（不解析语法, 只做配对与名字比对; 正确跳过 // 与 /* */ 注释、
' " ` 字符串、正则字面量），挂进 CI：改坏就红。

用法:
    python3 scripts/jscheck.py web_gateway/static/app.js
退出码 0 = 两遍都正常；1 = 有问题（逐条打印行号）。
"""

import re
import sys


def check(path):
    """返回 (ok, lines_or_errs)。"""
    src = open(path, encoding="utf-8").read()
    i, n = 0, len(src)
    line = 1
    stack = []
    pairs = {")": "(", "]": "[", "}": "{"}
    prev_sig = ""            # 上一个有意义的字符（判断 / 是除号还是正则开始）
    errs = []

    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue
        if c in "'\"`":
            q, start = c, line
            i += 1
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == "\n":
                    if q == "`":
                        line += 1
                        i += 1
                        continue
                    errs.append(f"line {start}: {q} 字符串没有闭合")
                    break
                if src[i] == q:
                    i += 1
                    break
                i += 1
            prev_sig = q
            continue
        if c == "/" and prev_sig in "(,=:[!&|?{};+*%":
            i += 1                                  # 正则字面量
            in_cls = False
            while i < n:
                if src[i] == "\\":
                    i += 2
                    continue
                if src[i] == "[":
                    in_cls = True
                elif src[i] == "]":
                    in_cls = False
                elif src[i] == "/" and not in_cls:
                    i += 1
                    break
                elif src[i] == "\n":
                    errs.append(f"line {line}: 正则字面量没有闭合")
                    break
                i += 1
            prev_sig = "/"
            continue
        if c in "([{":
            stack.append((c, line))
        elif c in ")]}":
            if not stack or stack[-1][0] != pairs[c]:
                errs.append(f"line {line}: 多余的 {c}")
            else:
                stack.pop()
        if not c.isspace():
            prev_sig = c
        i += 1

    for ch, ln in stack:
        errs.append(f"line {ln}: {ch} 没有闭合")
    return errs, line


# ---------------------------- ② "用了但没声明" 的标识符 ----------------------------
# JS 关键字 + 环境内建（浏览器/标准库）。只为了让"可疑名字"这一栏保持在 0 —— 名单里
# 少一个内建对象就会误报, 所以这里宁可写全一点; 真误报了就把名字补进来, 别去关掉检查。
KEYWORDS = set("""let const var function return if else for while do switch case break
continue new typeof instanceof delete in of this null true false undefined class try
catch finally throw await async yield static get set default export import from void
super extends arguments""".split())

BUILTINS = set("""document window console Math JSON Object Array String Number Boolean
Date RegExp Error Promise Map Set WeakMap WeakSet Symbol fetch setTimeout clearTimeout
setInterval clearInterval requestAnimationFrame cancelAnimationFrame performance
localStorage sessionStorage navigator location history URL URLSearchParams Blob File
FileReader FormData Image alert confirm prompt encodeURIComponent decodeURIComponent
encodeURI decodeURI parseInt parseFloat isNaN isFinite Infinity NaN globalThis
AbortController TextDecoder TextEncoder CustomEvent Event PointerEvent MouseEvent
KeyboardEvent ResizeObserver IntersectionObserver MutationObserver queueMicrotask
atob btoa crypto getComputedStyle matchMedia scrollTo scrollBy Element HTMLElement
Node NodeList SVGElement EventTarget Proxy Reflect Intl structuredClone devicePixelRatio
innerWidth innerHeight screen frames self parent top print stop""".split())

IDENT = re.compile(r"[A-Za-z_$][\w$]*")


def strip_literals(src):
    """与 check() 同样的扫描规则, 但把注释/字符串/正则**替换成空格**(保留换行)。

    这样后面就能安全地对"纯代码"跑正则, 不会把注释里的中文、字符串里的 'let x' 当成代码。
    """
    out = list(src)
    i, n = 0, len(src)
    prev = ""
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out[i] = " "; i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                if src[i] != "\n": out[i] = " "
                i += 1
            while i < n and src[i] != "/":
                out[i] = " "; i += 1
            if i < n: out[i] = " "; i += 1
            continue
        if c in "'\"`":
            q = c; i += 1
            while i < n:
                if src[i] == "\\":
                    out[i] = " "
                    if i + 1 < n and src[i + 1] != "\n": out[i + 1] = " "
                    i += 2; continue
                if src[i] == q:
                    break
                if src[i] != "\n": out[i] = " "
                i += 1
            if i < n: out[i] = " "; i += 1
            prev = q; continue
        if c == "/" and prev in "(,=:[!&|?{};+*%":
            out[i] = " "; i += 1                        # 正则字面量
            in_cls = False
            while i < n:
                if src[i] == "\\":
                    out[i] = " "; i += 2; continue
                if src[i] == "[": in_cls = True
                elif src[i] == "]": in_cls = False
                elif src[i] == "/" and not in_cls: break
                elif src[i] == "\n": break
                out[i] = " "; i += 1
            if i < n: out[i] = " "; i += 1
            while i < n and src[i].isalpha():            # 正则的 flag(g/i/m/s/u/y) 不是变量
                out[i] = " "; i += 1
            prev = "x"; continue
        if not c.isspace(): prev = c
        i += 1
    return "".join(out)


def declared_names(code):
    """声明过的名字: let/const/var/function + 多声明符 + 形参 + 箭头参数 + catch + `H = $(...)`。"""
    names = set()
    for m in re.finditer(r"\b(?:let|const|var|function)\s+([A-Za-z_$][\w$]*)", code):
        names.add(m.group(1))
    for m in re.finditer(r"\b(?:let|const|var)\b", code):      # let a = 1, b = 2, c;
        i, depth = m.end(), 0
        while i < len(code):
            c = code[i]
            if c in "([{": depth += 1
            elif c in ")]}":
                if depth == 0: break
                depth -= 1
            elif c == ";" and depth == 0: break
            elif c == "=" and depth == 0:
                j = i + 1                                      # 跳过初始化表达式
                while j < len(code):
                    if code[j] in "([{": depth += 1
                    elif code[j] in ")]}": depth -= 1
                    elif code[j] in ",;" and depth == 0: break
                    elif code[j] == "\n" and depth == 0 and j > i + 1: break
                    j += 1
                i = j
                if i < len(code) and code[i] == ",":
                    k = i + 1
                    while k < len(code) and code[k].isspace(): k += 1
                    mm = IDENT.match(code, k)
                    if mm: names.add(mm.group(0))
                continue
            i += 1
    for m in re.finditer(r"\bfunction\b[^(]*\(([^)]*)\)", code):     # 函数形参
        for p in m.group(1).split(","):
            if IDENT.fullmatch(p.strip() or ""): names.add(p.strip())
    for m in re.finditer(r"\(([^()]*)\)\s*=>", code):                # (a, b) => / (x) =>
        for p in m.group(1).split(","):
            if IDENT.fullmatch(p.strip() or ""): names.add(p.strip())
    for m in re.finditer(r"([A-Za-z_$][\w$]*)\s*=>", code):          # x => (单个参数免括号)
        names.add(m.group(1))
    for m in re.finditer(r"\bcatch\s*\(\s*([A-Za-z_$][\w$]*)", code):
        names.add(m.group(1))
    for m in re.finditer(r"([A-Za-z_$][\w$]*)\s*=\s*\$\(", code):    # 本仓库的手柄写法
        names.add(m.group(1))
    return names


def used_names(code):
    """用到的名字 → 首次出现的行号。跳过属性访问(obj.xx)与对象字面量的 key({xx: 1})。"""
    used = {}
    for m in IDENT.finditer(code):
        name = m.group(0)
        if name in KEYWORDS or name in BUILTINS: continue
        b = m.start() - 1
        while b >= 0 and code[b].isspace(): b -= 1
        if b >= 0 and code[b] in ".\\": continue
        a = m.end()
        while a < len(code) and code[a].isspace(): a += 1
        if a < len(code) and code[a] == ":":                        # 疑似对象 key
            p = m.start() - 1
            while p >= 0 and code[p].isspace(): p -= 1
            if p >= 0 and code[p] in "{,": continue
        used.setdefault(name, code[:m.start()].count("\n") + 1)
    return used


def check_idents(src):
    """返回 (可疑名单, 声明数, 用到数)。可疑 = 用到但既没声明、也不在内建名单里。"""
    code = strip_literals(src)
    d, u = declared_names(code), used_names(code)
    miss = sorted((ln, k) for k, ln in u.items() if k not in d)
    return miss, len(d), len(u)


def main(argv):
    paths = argv[1:]
    if not paths:
        print("用法: python3 scripts/jscheck.py <file.js> [更多文件…]")
        return 2
    bad = 0
    for p in paths:
        try:
            src = open(p, encoding="utf-8").read()
        except OSError as e:
            print(f"FAIL {p}: {e}")
            bad += 1
            continue
        errs, lines = check(p)
        miss, nd, nu = check_idents(src)
        if errs or miss:
            print(f"FAIL {p}")
            for e in errs:
                print("  " + e)
            for ln, name in miss:
                print(f"  line {ln}: {name} 用了但没声明（手柄名拼错? 或漏了 let/const/function）")
            bad += 1
        else:
            print(f"OK: {p} 括号/引号配平, 注释与字符串闭合正常 ({lines} 行); "
                  f"{nd} 个声明全对得上, {nu} 个用到的名字无失踪")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
