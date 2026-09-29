#!/usr/bin/env bash
# ============================================================
# scripts/bench.sh — CVInfer-Gate 吞吐基准 (性能改造专用)
# ------------------------------------------------------------
# 口径(见 PROJECT_NOTES §20.12):
#   固定窗口跑一段时间, 用**日志时间戳**算真实窗口:
#       吞吐 = processed / (收到信号 - 服务就绪)
#   ⚠️ 不能用 timeout 的秒数当分母(它从进程启动算, 含 ~0.3s 初始化, 会低估 ~8%)
#
# 读数据铁律:
#   1) 先看"解码速率": 低于 ~220fps 说明整机被降频(功耗/温度/后台负载),
#      该次数据作废、重跑, 不要当基线。(实测抓到过一次 193fps -> 吞吐同步掉到 24.1)
#   2) 基线参考: 8 vCPU / pool=2 / FP32 / 默认线程 = 30.2 ± 0.7 fps (源 29.96 fps)
#
# 用法:
#   bash scripts/bench.sh [次数, 默认3] [窗口秒数, 默认5]
# ============================================================
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build"
CFG="$REPO/config/config.yaml"
MCFG="$REPO/config/model_config.yaml"
N="${1:-3}"
WIN="${2:-5}"

cd "$BUILD" || { echo "找不到 $BUILD"; exit 1; }
[ -x ./CVInfer-Gate ] || { echo "找不到 ./CVInfer-Gate, 先编译"; exit 1; }

echo "配置: $CFG"
echo "      $MCFG"
echo "次数: $N   窗口: ${WIN}s(实际窗口以日志时间戳为准)"
echo
echo "--- 本次实际生效的关键参数(防止“改了一半就开跑”) ---"
grep -E "pool_size|num_threads|performance_mode" "$MCFG" | grep -vE "^[[:space:]]*#" | sed "s/^/  model_config: /"
grep -n "worker_threads" "$CFG" | sed "s/^/  config.yaml : /"
echo

for i in $(seq 1 "$N"); do
    log="/tmp/bench_$i.log"
    # 后台启动 -> 采样 CPU(并发度) -> 发 SIGTERM 优雅关闭(顺便拿到准确窗口)
    ./CVInfer-Gate --config "$CFG" --model-config "$MCFG" > "$log" 2>&1 &
    pid=$!
    sleep 1
    c0=$(( $(awk '{print $14+$15}' /proc/$pid/stat) ))
    sleep "$(( WIN - 2 ))"
    c1=$(( $(awk '{print $14+$15}' /proc/$pid/stat) ))
    cpu=$(( ((c1 - c0) * 100) / ($(getconf CLK_TCK) * (WIN - 2)) ))
    kill -TERM "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null

    serve=$(grep -m1 "服务就绪"   "$log" | cut -d " " -f1,2)
    sig=$(grep   -m1 "收到信号"   "$log" | cut -d " " -f1,2)
    proc=$(grep  -m1 -o "processed=[0-9]*" "$log" | cut -d = -f2)
    dec=$(grep   -m1 -o "decoded=[0-9]*"   "$log" | cut -d = -f2)

    if [ -z "${serve:-}" ] || [ -z "${sig:-}" ] || [ -z "${proc:-}" ]; then
        echo "第 $i 次: 日志缺少关键行 (见 $log)"
        continue
    fi

    s=$(date -d "$serve" +%s%N)
    e=$(date -d "$sig"   +%s%N)
    d=$(( (e - s) / 1000000 ))          # 窗口(毫秒)
    if [ "$d" -lt 500 ] || [ "$d" -gt 60000 ]; then
        echo "第 $i 次: 窗口异常 d=${d}ms  serve=[$serve]  sig=[$sig]"
        continue
    fi

    f=$(( proc * 10000 / d ))           # 吞吐 x10
    r=$(( dec  * 10000 / d ))           # 解码 x10
    printf "第 %d 次: 窗口=%d.%03ds  decoded=%d  processed=%d  ->  %d.%d fps  (解码 %d.%d fps, 并发度 %d%%)\n" \
        "$i" "$((d / 1000))" "$((d % 1000))" "$dec" "$proc" "$((f / 10))" "$((f % 10))" "$((r / 10))" "$((r % 10))" "$cpu"

    # 从日志里回显**实际生效**的参数(防止“配置没生效”类乌龙)
    printf "          [生效] %s | %s | %s\n" \
        "$(grep -m1 -oE "device=[^,]+, performance_mode=[^,]+, num_threads=[^ ]*" "$log")" \
        "$(grep -m1 -oE "引擎数量: [0-9]+" "$log")" \
        "$(grep -m1 -oE "pool=[0-9]+" "$log")"
done
