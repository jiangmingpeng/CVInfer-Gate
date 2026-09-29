#!/usr/bin/env bash
# ============================================================
# scripts/stack_probe.sh — 抓取运行中所有线程的调用栈（谁在等谁）
# ------------------------------------------------------------
# 为什么需要它:
#   thread_probe.sh 的逐线程 CPU 表只能告诉我们“有几个线程在烧 CPU、各占多少”,
#   但看不出 **它们在等什么**(等内存? 等 barrier? 等锁? 等引擎?)。
#   这个问题只能靠调用栈回答。
#
# 最想知道的三个数:
#   1) 有 **几个** worker 线程同时卡在 OpenVINOEngine::infer 里?
#        1 个 = 推理被串行化了;  4 个 = 真并发(那瓶颈就在 OV 池内部)
#   2) OV 的池线程有几个? (观察到的可疑数字是 6, 不是 4 也不是 8)
#   3) 池线程是在算(conv/gemm/池化帧), 还是在等(futex/cond_wait)?
#
# 用法: bash scripts/stack_probe.sh          (需要 gdb)
#       没装的话: sudo apt-get update && sudo apt-get install -y gdb
# 产物: /tmp/stacks.txt (完整栈, 线程号可与 thread_probe.sh 的表对照)
# ============================================================
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO/build" || { echo "找不到 $REPO/build"; exit 1; }
[ -x ./CVInfer-Gate ] || { echo "找不到 ./CVInfer-Gate"; exit 1; }

if ! command -v gdb >/dev/null 2>&1; then
    echo "需要 gdb:  sudo apt-get update && sudo apt-get install -y gdb"
    exit 1
fi

echo "启动 CVInfer-Gate, 3s 后由 gdb 中断并抓取全部线程栈..."

# 说明: 直接用 `gdb -p <pid>` 在 Ubuntu/WSL 上会被 Yama(ptrace_scope=1) 拒绝
#       —— gdb 与目标是“兄弟进程”而非父子。让 gdb 亲自启动目标即可绕过。
timeout -s INT 3 gdb -batch -q \
    -ex "set pagination off" \
    -ex "set confirm off" \
    -ex "handle SIGINT stop nopass" \
    -ex "run" \
    -ex "thread apply all bt" \
    --args ./CVInfer-Gate --config "$REPO/config/config.yaml" \
                         --model-config "$REPO/config/model_config.yaml" \
    > /tmp/stacks.txt 2>&1

pkill -f "CVInfer-Gate --config" 2>/dev/null

if [ "$(wc -l < /tmp/stacks.txt)" -lt 20 ]; then
    echo "!! 抓栈失败, gdb 原始输出如下(前 25 行):"
    sed -n '1,25p' /tmp/stacks.txt
    exit 1
fi

echo
echo "== 关键计数 =="
printf "  %-52s %s\n" "同时卡在 infer 里的线程数(1=串行, 4=真并发):" \
    "$(grep -cE 'OpenVINOEngine::infer|ov::InferRequest' /tmp/stacks.txt)"
printf "  %-52s %s\n" "VideoPipeline worker 线程数:" \
    "$(grep -cE 'workerLoop|VideoPipeline::start' /tmp/stacks.txt)"
printf "  %-52s %s\n" "OV 池线程数 (Executor/threading/ThreadPool):" \
    "$(grep -cE 'CPUStreamsExecutor|ov::threading|Executor|ThreadPool' /tmp/stacks.txt)"
printf "  %-52s %s\n" "当前在等锁/等唤醒 (futex/cond_wait):" \
    "$(grep -cE 'futex|cond_wait|pthread_cond' /tmp/stacks.txt)"
printf "  %-52s %s\n" "在解码 (VideoCapture/ffmpeg/avcodec):" \
    "$(grep -cE 'VideoCapture|avcodec|av_read|libav' /tmp/stacks.txt)"
printf "  %-52s %s\n" "堆栈文件总行数:" "$(wc -l < /tmp/stacks.txt)"

echo
echo "== 逐线程关键帧 =="
grep -E "^Thread [0-9]+|^#[0-9]+ .*(ov::|openvino|VideoPipeline|YoloDetector|OpenVINOEngine|InferenceEngine|ThreadSafe|VideoCapture|avcodec|ffmpeg|libav|sinkLoop|decodeLoop|workerLoop|futex|cond_wait|pthread_cond|cv::)" \
    /tmp/stacks.txt | head -200

echo
echo "(完整栈: /tmp/stacks.txt ; 线程号可与 thread_probe.sh 的表对照)"
