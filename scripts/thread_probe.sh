#!/usr/bin/env bash
# ============================================================
# scripts/thread_probe.sh — 逐线程 CPU 透视（性能诊断用）
# ------------------------------------------------------------
# 为什么需要它（实测背景）:
#   num_threads = 1 / 2 / 4 / 8  ->  并发度 = 215 / 318 / 514 / 512 %
#   ⇒ 推理并行度被“钉”在物理核数(4)上, num_threads>4 完全无效
#   ⇒ 怀疑 OpenVINO CPU 插件默认拒绝超线程(SMT), 4 个逻辑核永远闲着
#
# 本脚本直接回答三个问题(不靠猜):
#   1) 进程里到底有多少个线程? 名字是什么?（OV 的池线程有特征名）
#   2) 哪几个线程真的在烧 CPU, 各占多少?
#   3) 推理线程池到底是 4 个还是 8 个?
#
# 用法:  bash scripts/thread_probe.sh [采样秒数, 默认4]
# 输出:  逐线程 CPU 占用表 + 线程名汇总 + 总线程数
# ============================================================
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$REPO/build"
CFG="$REPO/config/config.yaml"
MCFG="$REPO/config/model_config.yaml"
WIN="${1:-4}"
CLK="$(getconf CLK_TCK)"

cd "$BUILD" || { echo "找不到 $BUILD"; exit 1; }
[ -x ./CVInfer-Gate ] || { echo "找不到 ./CVInfer-Gate"; exit 1; }

echo "启动 CVInfer-Gate (跳过启动阶段后采样 ${WIN}s)..."
./CVInfer-Gate --config "$CFG" --model-config "$MCFG" > /tmp/probe.log 2>&1 &
pid=$!
sleep 1.5                      # 跳过启动/编译阶段(窗口落在视频处理期内)

snap() {                       # 每行: tid \t comm \t cputicks
    for t in /proc/$pid/task/*; do
        [ -r "$t/stat" ] || continue
        printf "%s\t%s\t%s\n" "$(basename "$t")" \
            "$(cat "$t/comm" 2>/dev/null)" \
            "$(awk '{print $14+$15}' "$t/stat" 2>/dev/null)"
    done
}
snap > /tmp/probe_a.txt
sleep "$WIN"
snap > /tmp/probe_b.txt

kill -TERM "$pid" 2>/dev/null
wait "$pid" 2>/dev/null

echo
echo "== 逐线程 CPU 占用 (窗口 ${WIN}s; 100% = 1 个逻辑核跑满) =="
awk -v clk="$CLK" -v w="$WIN" '
    FNR == NR { a[$1] = $3; next }
    ($1 in a) { printf "%8s\t%-26s\t%6.1f%%\n", $1, $2, ($3 - a[$1]) * 100.0 / (clk * w) }
' /tmp/probe_a.txt /tmp/probe_b.txt | sort -k3 -rn

echo
echo "== 到底有几个线程在干活 =="
awk -v clk="$CLK" -v w="$WIN" '
    FNR == NR { a[$1] = $3; next }
    ($1 in a) { p = ($3 - a[$1]) * 100.0 / (clk * w); if (p > 80) hot++; else if (p > 20) warm++ }
    END { printf "  >=80%%(基本跑满): %d 个   20~80%%: %d 个\n", hot + 0, warm + 0 }
' /tmp/probe_a.txt /tmp/probe_b.txt

echo
echo "== 线程名汇总(有几个就叫什么名) =="
awk -F'\t' '{print $2}' /tmp/probe_b.txt | sort | uniq -c | sort -rn

echo
echo "== 汇总 =="
echo "进程总线程数 : $(wc -l < /tmp/probe_b.txt)"
echo "逻辑核(nproc): $(nproc)"
lscpu 2>/dev/null | grep -E "^ *(Core\(s\) per socket|Socket\(s\)|Thread\(s\) per core|CPU\(s\)):" | sed 's/^/  /'
echo
echo "判读提示:"
echo "  - OV 的推理线程池线程数若只有物理核数(4), 而 num_threads 写的是 8"
echo "    => 证实“超线程被拒”, 下一步就该上 num_streams / performance_mode: throughput"
echo "  - 若 4 个 worker 线程 CPU 都接近 0%  => worker 在等锁/等引擎"
