"""CVInfer-Gate 的 Python BFF 网关（Flask：HTTP ⇄ gRPC）。

启动方式（推荐一键，会自动用仓库根的 .venv 并补齐依赖）：
    bash web_gateway/run.sh

手动启动：
    source .venv/bin/activate
    pip install -r web_gateway/requirements.txt
    python web_gateway/app.py

⚠ 如果直接 `python app.py` 报 ModuleNotFoundError：不是代码坏了，
   而是用错了解释器（依赖装在 .venv 里，没激活就会用系统 Python）。
   下面的 _check_deps() 会把「缺什么 / 怎么装」直接打成中文，
   而不是丢一句英文堆栈让人猜。
"""

import base64
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from collections import deque

# 让下面三种启法都能 import 到同目录的 pb2 桩文件：
#   python web_gateway/app.py / cd web_gateway && python app.py / python -m web_gateway.app
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)


def _check_deps():
    """启动前自检依赖：缺哪个、该用什么解释器、怎么装，用中文说清楚。"""
    need = (
        ("flask", "flask"),
        ("grpc", "grpcio"),
        ("cv2", "opencv-python-headless"),
        ("numpy", "numpy"),
    )
    missing = []
    for mod, pkg in need:
        try:
            __import__(mod)
        except ImportError:
            missing.append(pkg)
    if not missing:
        return

    in_venv = sys.prefix != getattr(sys, "base_prefix", sys.prefix)
    venv_dir = os.path.join(os.path.dirname(_HERE), ".venv")

    lines = [
        "",
        "=" * 70,
        "[web_gateway] 启动失败：当前 Python 缺少依赖 -> " + ", ".join(missing),
        "              当前解释器  : " + sys.executable,
        "              在虚拟环境中: " + ("是" if in_venv else "否"),
        "-" * 70,
    ]
    if not in_venv and os.path.isdir(venv_dir):
        lines += [
            "这个仓库其实已经建好了虚拟环境，你只是没激活它：",
            "  " + venv_dir,
            "",
            "任选一种启动方式：",
            "",
            "  A) 一键启动（推荐；自动激活 + 缺依赖自动装）：",
            "       bash web_gateway/run.sh",
            "",
            "  B) 手动：",
            "       source .venv/bin/activate",
            "       pip install -r web_gateway/requirements.txt",
            "       python web_gateway/app.py",
        ]
    else:
        lines += [
            "任选一种启动方式：",
            "",
            "  A) 一键启动（推荐；自动建 .venv + 装依赖）：",
            "       bash web_gateway/run.sh",
            "",
            "  B) 手动：",
            "       python3 -m venv .venv && source .venv/bin/activate",
            "       pip install -r web_gateway/requirements.txt",
            "       python web_gateway/app.py",
        ]
    lines += ["=" * 70, ""]
    print("\n".join(lines), file=sys.stderr)
    sys.exit(2)


_check_deps()

import numpy as np          # noqa: E402  (必须在 _check_deps 之后)
import cv2                  # noqa: E402
import grpc                 # noqa: E402
from flask import Flask, jsonify, render_template, request, Response, send_file  # noqa: E402

# 座位配置的**文本层**读写单独成模块（只依赖标准库）—— 见 seats_yaml.py 顶部说明:
# 那里有一条必须被 CI 守住的约定（只换 occupancy.seats 这一段, 其余字节含注释一字不动）。
import seats_yaml            # noqa: E402  (与 pb2 一样: 需要上面的 sys.path)

# pb2 桩文件是同目录下由 proto 生成的（已入库）。缺了就明确告诉怎么重新生成。
try:
    from inference_pb2 import DetectRequest
    from inference_pb2_grpc import DetectionServiceStub
except ImportError as _exc:
    print(
        "\n" + "=" * 70 + "\n"
        "[web_gateway] 启动失败：gRPC 桩文件导入不了 -> " + str(_exc) + "\n"
        "  （需要 web_gateway/inference_pb2.py 与 inference_pb2_grpc.py）\n"
        "重新生成（在仓库根目录执行）：\n"
        "  python -m grpc_tools.protoc -I proto \\\n"
        "      --python_out=web_gateway --grpc_python_out=web_gateway \\\n"
        "      proto/inference.proto\n"
        + "=" * 70 + "\n",
        file=sys.stderr,
    )
    sys.exit(3)

# ---------------------- 运行参数全部来自环境变量 ----------------------
# 去除硬编码 IP：不再把 C++ 服务地址写死在源码里。
# 可选：若安装了 python-dotenv，则自动加载同目录 .env（本地开发方便）。
# 注意这里**显式指定路径**：否则从别的目录启动时，会按 CWD 找 .env 而读不到。
try:
    from dotenv import load_dotenv
    load_dotenv(os.path.join(_HERE, ".env"))
except ImportError:
    pass


def _env_str(name, default):
    value = os.environ.get(name)
    return value if value not in (None, "") else default


def _env_int(name, default):
    value = os.environ.get(name)
    if value in (None, ""):
        return default
    try:
        return int(value)
    except ValueError:
        raise ValueError(f"环境变量 {name}={value!r} 不是合法整数")


# C++ gRPC 服务地址 host:port（默认本地，避免写死任何真实 IP）
GRPC_SERVER = _env_str("GRPC_SERVER", "localhost:50051")
# 调用超时(ms)，与 C++ 端 grpc.timeout_ms 对齐
GRPC_TIMEOUT_MS = _env_int("GRPC_TIMEOUT_MS", 5000)
# 收发消息上限(MB)，与 C++ 端 grpc.max_message_size_mb 对齐（默认 4MB 会挡住大图）
GRPC_MAX_MSG_MB = _env_int("GRPC_MAX_MSG_MB", 16)
# 主服务(50051)鉴权 token：非空则每次调用带 authorization: Bearer <token>
# 留空 => 不带任何 metadata（要求服务端也没开鉴权，与改动前完全一致）
GRPC_AUTH_TOKEN = _env_str("GRPC_AUTH_TOKEN", "")
# Flask 监听地址与端口
WEB_HOST = _env_str("WEB_HOST", "0.0.0.0")
WEB_PORT = _env_int("WEB_PORT", 8080)

# ---------------------- 结果产物（给页面看"跑完到底出了什么"） ----------------------
# C++ 主程序把带框结果视频写成 output.avi（相对**启动目录**解析），
# 一般从 build/ 启动 ⇒ 落在 <仓库根>/build/output.avi。
_REPO_ROOT = os.path.dirname(_HERE)
RESULT_DIR = _env_str("RESULT_DIR", os.path.join(_REPO_ROOT, "build"))
RESULT_VIDEO = _env_str("RESULT_VIDEO", "output.avi")
# 日志文件（用来把"占座/告警事件"抽出来给人看）；空 = 自动找 <RESULT_DIR>/logs/cvinfer.log
RESULT_LOG = _env_str("RESULT_LOG", "")
# 转码用：output.avi 是 MJPEG，浏览器放不了，需要 ffmpeg 转 mp4（结果会缓存）
FFMPEG_BIN = _env_str("FFMPEG_BIN", "ffmpeg")
# C++ 侧配置：用来读 occupancy.seats，好在画面上标出"事件发生在哪一块"。
# 换配置就设 CVINFER_CONFIG（例如指向 config.yaml）。
CONFIG_PATH = _env_str("CVINFER_CONFIG", os.path.join(_REPO_ROOT, "config", "config.test.yaml"))


def _sync_build_config_mirror(path=None):
    """把写回后的配置同步到 build/config/<同名>（那边已有一份时才同步）。

    为什么需要它: C++ 主程序按**启动目录**解析相对路径, 而商家/文档的启动命令是
    `cd build && ./CVInfer-Gate --config config/config.test.yaml` ⇒ 程序读的是
    **构建时拷贝**出来的 `build/config/config.test.yaml`；网页写回的却是
    `<REPO_ROOT>/config/config.test.yaml`。两份不同步时, 网页里 zone 画得再准,
    程序也永远读不到 —— 实测表现就是「跑完整段视频仍 occupied_events=0 ⇒ 0 复核
    ⇒ VLM 一个请求都收不到」。写回后顺手刷新镜像, 让「网页编辑的那份」与
    「程序按启动目录读到的那份」始终一致，商家现有的启动命令不必改。

    返回同步到的镜像路径；None = 没有镜像 / 无需同步（绝不去动别的文件）。
    """
    src = path or CONFIG_PATH
    if not src or not _REPO_ROOT:
        return None
    src_abs = os.path.abspath(src)
    # 护栏: 只同步「仓库 config/ 里的那份 → build/config 镜像」。
    # 绝不让**临时文件**(如自检脚本用的临时副本 config.yaml)借同名覆盖 build 里的真配置。
    if os.path.dirname(src_abs) != os.path.abspath(os.path.join(_REPO_ROOT, "config")):
        return None
    tgt = os.path.join(_REPO_ROOT, "build", "config", os.path.basename(src_abs))
    if os.path.abspath(tgt) == src_abs:
        return None                       # 编辑的就是 build 里的那份, 不用镜像
    if not os.path.isfile(tgt):
        return None                       # 没有镜像(未构建 / 配置名不同) ⇒ 不新建
    try:
        shutil.copy2(src_abs, tgt)
        return tgt
    except OSError:
        return None


def _same_file_content(a, b):
    """a 与 b 是否指向**内容相同**的现存文件。

    用于「程序启动目录读到的那份」与「网页在编辑的那份」是否真是同一份：
    只比文件名会漏判（两份都叫 config.test.yaml, 但一份在 build/config/、
    一份在仓库根 config/）—— 实测就是这里骗过了诊断面板。这里展开真实路径后
    逐字节比对, 内容一致才算「同一份」。
    """
    try:
        if not (a and b and os.path.isfile(a) and os.path.isfile(b)):
            return False
        if os.path.realpath(a) == os.path.realpath(b):
            return True
        with open(a, "rb") as fa, open(b, "rb") as fb:
            return fa.read() == fb.read()
    except OSError:
        return False


# ---------------------- 座位标定探针（上传视频 → 在线热力图） ----------------------
# 「哪个框算进哪一格、每格算几次」由 C++ 的 --seat-probe 算完（口径与线上占座判定同源），
# 这里只负责**跑它 + 把结果画出来** —— 绝不在 Python 里重算热力，否则热力图会骗人。
CVINFER_BIN = _env_str("CVINFER_BIN", os.path.join(_REPO_ROOT, "build", "CVInfer-Gate"))
MODEL_CONFIG_PATH = _env_str("CVINFER_MODEL_CONFIG",
                             os.path.join(_REPO_ROOT, "config", "model_config.yaml"))
# 每隔 N 帧采样一次（标定不需要逐帧；调大只影响采样密度，不改变归属口径）
SEAT_PROBE_STRIDE = _env_int("SEAT_PROBE_STRIDE", 5)
# 探针是"把整段视频跑一遍模型"，比一次图片检测慢得多，超时给宽一些
SEAT_PROBE_TIMEOUT = _env_int("SEAT_PROBE_TIMEOUT", 900)

app = Flask(__name__)

# 1. 连接 C++ gRPC 服务（地址/超时/消息上限均由环境变量注入）
channel = grpc.insecure_channel(
    GRPC_SERVER,
    options=[
        ("grpc.max_receive_message_length", GRPC_MAX_MSG_MB * 1024 * 1024),
    ],
)
stub = DetectionServiceStub(channel)


def _grpc_metadata():
    """主服务鉴权 metadata：token 非空才带。

    服务端(AuthGuard)比的是**整串** "Bearer " + token —— 大小写敏感、没有尾空格
    容错，所以这里必须精确拼同一串（与 tests/test_grpc_client.cpp 完全一致）。
    """
    if not GRPC_AUTH_TOKEN:
        return ()
    return (("authorization", f"Bearer {GRPC_AUTH_TOKEN}"),)


# ---------------------- 展示层工具（纯 UI，不改变推理/鉴权行为） ----------------------

# 按 class_id 取色：同一类别在结果图里颜色稳定（BGR，OpenCV 顺序）
_PALETTE = [
    (56, 189, 248), (52, 211, 153), (251, 191, 36), (248, 113, 113),
    (167, 139, 250), (244, 114, 182), (34, 211, 238), (163, 230, 53),
]


def _annotate(img, detections):
    """在图上画框 + 带底色的标签条。"""
    if img is None:
        return img
    for det in detections:
        color = _PALETTE[det.class_id % len(_PALETTE)]
        x1, y1, x2, y2 = det.x1, det.y1, det.x2, det.y2
        cv2.rectangle(img, (x1, y1), (x2, y2), color, 2)
        text = f"{det.label} {det.confidence * 100:.0f}%"
        (tw, th), base = cv2.getTextSize(text, cv2.FONT_HERSHEY_SIMPLEX, 0.55, 2)
        ty = y1 - 8
        if ty - th < 0:  # 贴顶时把标签挪到框下方
            ty = y2 + th + 8
        cv2.rectangle(img, (x1, ty - th - 6), (x1 + tw + 12, ty + base), color, -1)
        cv2.putText(img, text, (x1 + 6, ty), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                    (255, 255, 255), 2)
    return img


def _run_detect(img_bytes):
    """调用 C++ gRPC。成功返回 (response, None)，失败返回 (None, (msg, http_code))。"""
    try:
        response = stub.Detect(
            DetectRequest(image_data=img_bytes),
            timeout=GRPC_TIMEOUT_MS / 1000.0,
            metadata=_grpc_metadata(),   # 空 token => ()，不带 metadata
        )
    except grpc.RpcError as e:
        # 鉴权失败最常见的原因：网关与服务端 env 不一致(或只配了一边)
        if e.code() == grpc.StatusCode.UNAUTHENTICATED:
            return None, ("鉴权不通过 —— 网关与服务端必须用同一个 GRPC_AUTH_TOKEN"
                          f"（当前网关侧: {'已设置' if GRPC_AUTH_TOKEN else '未设置'}）", 401)
        return None, (f"C++ 服务调用失败: {e.details()}", 502)
    if not response.success:
        return None, (f"服务端推理失败: {response.message}", 500)
    return response, None


# 2. 主页：模板在 templates/index.html，样式/脚本在 static/
@app.route('/')
def index():
    return render_template(
        'index.html',
        grpc_server=GRPC_SERVER,
        auth_enabled=bool(GRPC_AUTH_TOKEN),
        max_msg_mb=GRPC_MAX_MSG_MB,
    )


# 3. 旧接口（保留兼容）：接收图片 -> 调 C++ -> 返回带框 JPEG
@app.route('/detect', methods=['POST'])
def detect():
    if 'image' not in request.files:
        return "未上传图片", 400
    img_bytes = request.files['image'].read()
    response, err = _run_detect(img_bytes)
    if err:
        return err[0], err[1]

    img = cv2.imdecode(np.frombuffer(img_bytes, np.uint8), cv2.IMREAD_COLOR)
    _annotate(img, response.detections)
    ok, buffer = cv2.imencode('.jpg', img)
    if not ok:
        return "结果编码失败", 500
    return Response(buffer.tobytes(), mimetype='image/jpeg')


# 4. 页面用接口：返回 JSON —— 带框图(dataURL) + 结构化检测框
@app.route('/api/detect', methods=['POST'])
def api_detect():
    if 'image' not in request.files:
        return jsonify(ok=False, error="未上传图片"), 400
    img_bytes = request.files['image'].read()
    if not img_bytes:
        return jsonify(ok=False, error="图片内容为空"), 400

    img = cv2.imdecode(np.frombuffer(img_bytes, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        return jsonify(ok=False, error="无法解码图片（请上传 PNG / JPG）"), 400

    response, err = _run_detect(img_bytes)
    if err:
        return jsonify(ok=False, error=err[0]), err[1]

    _annotate(img, response.detections)
    ok, buffer = cv2.imencode('.jpg', img, [int(cv2.IMWRITE_JPEG_QUALITY), 90])
    image_url = ("data:image/jpeg;base64,"
                 + base64.b64encode(buffer.tobytes()).decode('ascii')) if ok else ""

    detections = [{
        "class_id": det.class_id,
        "label": det.label,
        "confidence": round(float(det.confidence), 4),
        "x1": det.x1, "y1": det.y1, "x2": det.x2, "y2": det.y2,
        "width": det.x2 - det.x1,
        "height": det.y2 - det.y1,
    } for det in response.detections]

    return jsonify(ok=True, message=response.message, count=len(detections),
                   detections=detections, image=image_url)


# ---------------------- 结果产物查看（流水线的"最后输出"在这里体现） ----------------------

def _human_size(n):
    for unit, div in (("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= div:
            return f"{n / div:.1f} {unit}"
    return f"{n} B"


def _file_info(path, note=""):
    if not path or not os.path.isfile(path):
        return {"exists": False, "name": os.path.basename(path) if path else "", "note": note}
    st = os.stat(path)
    return {
        "exists": True,
        "name": os.path.basename(path),
        "size": st.st_size,
        "size_text": _human_size(st.st_size),
        "mtime": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(st.st_mtime)),
        "note": note,
    }


def _safe_under(dirpath, name):
    """把用户给的 name 限制在 dirpath 内（防 ../ 逃逸）。非法返回 None。"""
    if not name:
        return None
    base = os.path.realpath(dirpath)
    full = os.path.realpath(os.path.join(base, os.path.basename(name)))
    return full if (full == base or full.startswith(base + os.sep)) else None


def _find_log():
    if RESULT_LOG:
        return RESULT_LOG
    cand = os.path.join(RESULT_DIR, "logs", "cvinfer.log")
    return cand if os.path.isfile(cand) else None


def _other_log():
    """仓库根的 logs/cvinfer.log —— 它存在 ⇒ 程序是**从仓库根**启动的。

    这种启动方式下 `video.source_path: ../test.mp4` 必然打不开（相对路径漂移一格），
    而且日志/产物都落在仓库根，而网页读的是 <RESULT_DIR>/logs/… ⇒ 页面永远是空的。
    这条只能靠"另一份日志存在"推出来，日志自己不会说。
    """
    mine = _find_log()
    cand = os.path.join(_REPO_ROOT, "logs", "cvinfer.log")
    if os.path.isfile(cand) and os.path.realpath(cand) != os.path.realpath(mine or ""):
        return cand
    return None


# ---------------------- 「为什么一条告警都没有」的自证 ----------------------
# 实测踩过的坑: 商家画好座位、跑完视频, 页面只给一句「本轮未检测到占座」——
# 于是他开始猜（没识别到包? zone 画错了?），一下午白跑。
# 真相却**全都已经写在日志里**: 视频源打不开（一帧都没处理）/ 这轮用的还是旧框 /
# 候选被复核与推送闸门丢掉了……
# 所以这里只做一件事: 把日志里**原文**抽出来摆到页面上, 每条结论都附「日志原文」可展开核对。
# ⚠ 绝不在 Python 里重写占座判据 —— 命中/不命中、该不该报, 一律以 C++ 的日志原文为准。
_LOG_ROUND_START = "=== CVInfer-Gate 启动 ==="
_RE_OCC_ON = re.compile(r"占座判定: 已启用 \(座位数=(\d+), 物品类别=\[([^\]]*)\](.*)\)")
_RE_OCC_STATS = re.compile(r"占座统计:\s*frames=(\d+)\s+seats=(\d+)\s+occupied_events=(\d+)")
_RE_SEAT_OOB = re.compile(r"座位 \[([^\]]+)\] 区域超出画面 (\d+)x(\d+), 仅 (\d+)% 面积可见")
_RE_CFG_LOADED = re.compile(r"系统配置加载成功:\s*(\S+)")


def _occ_block(text):
    """取 occupancy 这一段（到下一个顶层键为止）—— 只用于**展示**配置里写了什么。"""
    m = re.search(r"^occupancy\s*:.*$", text, re.M)
    if not m:
        return ""
    rest = text[m.end():]
    end = re.search(r"^\S", rest, re.M)
    return rest[:end.start()] if end else rest


def _config_digest():
    """网页**正在编辑哪份配置**、它有没有 occupancy 段、里面写了什么（display-only）。

    这不是判定: 真值与口径一律以程序日志里的运行期输出为准（见 _diagnose 的「生效配置」那条）。
    但"我改的是不是程序读的那份文件"必须让商家一眼看到 —— 这是实测最容易白干的一处。
    """
    d = {"path": CONFIG_PATH, "exists": os.path.isfile(CONFIG_PATH),
         "has_occupancy": False, "enabled_text": None, "item_labels": None,
         "mtime": None, "error": None}
    if not d["exists"]:
        return d
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        d["mtime"] = time.strftime("%Y-%m-%d %H:%M:%S",
                                   time.localtime(os.path.getmtime(CONFIG_PATH)))
    except OSError as e:
        d["error"] = f"读配置失败: {e}"
        return d
    blk = _occ_block(text)
    d["has_occupancy"] = bool(blk)
    if not blk:
        return d
    m = re.search(r"^[ \t]+enabled\s*:\s*([^#\n]+)", blk, re.M)
    if m:
        d["enabled_text"] = m.group(1).strip()
    m = re.search(r"^[ \t]+item_labels\s*:\s*\[([^\]]*)\]", blk, re.M)
    if m:
        d["item_labels"] = [s.strip().strip("'\"") for s in m.group(1).split(",")
                            if s.strip()]
    return d


def _last_round(lines):
    """只留**最后一轮**（= 与当前 output.avi 对应的那次运行）。"""
    anchors = [i for i, ln in enumerate(lines) if _LOG_ROUND_START in ln]
    if not anchors:
        anchors = [i for i, ln in enumerate(lines) if "服务就绪" in ln]
    return lines[anchors[-1]:] if anchors else lines


def _verdict(level, title, detail="", evidence=None):
    """一条结论 + 它的出处（日志原文, 或"另一份文件"的路径）。前端原样展示, 不做二次加工。"""
    return {"level": level, "title": title, "detail": detail, "evidence": evidence}


def _diagnose(lines):
    """把"为什么事件列表是空的"从日志原文里读出来。每条都带原文行。

    只做**引用与归纳**, 不复算任何判据:
      · 有没有真处理过帧（视频源打开失败 / 有没有「服务就绪」「占座统计」）;
      · 规则层自己的结论（占座统计 occupied_events=N + 每座位那行 `[占座] …` 原文）;
      · **生效**的配置（`占座判定: 已启用 (座位数=…, 物品类别=[…])`—— 运行期真值, 不是文件里写的）;
      · 三道下游闸门（复核服务 / alert_on_failure / 告警推送 / 数据库降级）。
    """
    expected = _find_log() or os.path.join(RESULT_DIR, "logs", "cvinfer.log")
    if not lines:
        return [_verdict(
            "warn", "还没有可读的日志 —— 这台机器上可能从没跑过 C++ 主程序, 或日志落在了别处",
            "网页读的就是上面那行路径。启动方式：cd build && ./CVInfer-Gate "
            "--config config/config.test.yaml --model-config config/model_config.yaml "
            "（配置里的视频/模型/输出全是**相对启动目录**的路径, 只能在 build/ 下启动）。",
            expected)]

    def hits(s):
        return [ln for ln in lines if s in ln]

    v = []
    video_fail = hits("视频源打开失败")
    ready = hits("服务就绪")
    occ_off = hits("占座判定: 已禁用")
    occ_on = [ln for ln in lines if _RE_OCC_ON.search(ln)]
    review_down = hits("复核服务暂不可用")
    review_ready = hits("复核服务就绪")
    push_off = hits("告警推送: 已禁用")
    db_deg = hits("以【降级模式】启动")
    cfg_line = hits("系统配置加载成功")
    oob = [ln for ln in lines if _RE_SEAT_OOB.search(ln)]
    seat_reason = hits("[占座]")

    stats = None
    for ln in reversed(lines):                       # 收尾统计取**最后一次**
        if _RE_OCC_STATS.search(ln):
            stats = ln
            break

    if video_fail:
        v.append(_verdict(
            "err", "这轮一帧都没处理：视频源打不开",
            "没处理帧 ⇒ 检测/追踪/占座全都收不到输入, 事件列表必然是空的 —— 既不是"
            "「没识别到包」, 也不是 zone 画错。配置里的视频路径是**相对启动目录**解析的"
            "（config.test.yaml 里是 ../test.mp4）⇒ 必须在 build/ 下启动: "
            "cd build && ./CVInfer-Gate --config config/config.test.yaml "
            "--model-config config/model_config.yaml。",
            video_fail[-1]))
    elif not ready:
        v.append(_verdict(
            "warn", "这轮没跑完：日志里没有「服务就绪」",
            "程序可能还在启动中、启动即退出, 或被 Ctrl+C 提前打断（下面原文是它最后写的一行）。",
            lines[-1]))

    if occ_off:
        v.append(_verdict(
            "warn", "配置里 occupancy 没开（或这份文件里根本没有 occupancy 段）",
            "「占座判定: 已禁用」⇒ 规则层一个占座事件都不会产生。先确认网页编辑的那份配置"
            "就是程序加载的那份（见最上面那行路径）。", occ_off[-1]))

    if occ_on:
        m = _RE_OCC_ON.search(occ_on[-1])
        v.append(_verdict(
            "info", "生效的占座配置（运行期真值，不是文件里写的）：座位数=" + m.group(1)
            + "，物品类别=[" + m.group(2) + "]",
            "物品类别是**精确相等**匹配：检测器输出的是 COCO 的类别名（包只有 "
            "backpack / handbag 两种写法, 根本没有 \"bag\" 这个词）⇒ 列表里写了检测器"
            "**不会输出**的词, 那个类别就永远不参与占座判定。检测到了却不在列表里的类别"
            "（例如 bottle / cup）会被规则层有意忽略。",
            occ_on[-1]))

    if stats:
        m = _RE_OCC_STATS.search(stats)
        frames, seats_n, ev_n = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if ev_n == 0:
            v.append(_verdict(
                "warn", f"规则层结论：处理了 {frames} 帧、{seats_n} 个座位，占座事件 0 个",
                "既不是「没跑到」也不是「阈值太严」—— 这是规则层当场给出的结论。"
                "下面每行 `[占座] 座位 X: …` 就是它给的原因。", stats))
        else:
            v.append(_verdict(
                "ok", f"规则层产生了 {ev_n} 个占座事件（处理 {frames} 帧）",
                "事件要变成**告警**还得过复核这一关：看下面「复核服务」那条。", stats))
        for ln in seat_reason[-6:]:
            v.append(_verdict("info", "规则层对本轮某座位的原话", "", ln))
    else:
        v.append(_verdict(
            "warn", "没有「占座统计」收尾行 ⇒ 这轮没有正常跑完、或没有正常退出",
            "那一行只在程序**正常退出**（Ctrl+C）时写。跑完请按 Ctrl+C, 别 kill -9。",
            lines[-1] if lines else expected))

    for ln in oob[-4:]:                              # 越界框: 报出「哪一式多宽」, 不是笼统说画偏了
        m = _RE_SEAT_OOB.search(ln)
        v.append(_verdict(
            "warn", "座位 [" + m.group(1) + "] 有 " + str(100 - int(m.group(4))) + "% 露在画面外",
            "画面 " + m.group(2) + "x" + m.group(3) + " —— 露在画外的那一截永远不会有目标"
            "落进来。这不是「画得偏」而是「有一截在画外」, 按当前分辨率重画即可"
            "（网页 ④ 的探针会直接给建议 zone）。", ln))

    if occ_on or stats:                              # 下游闸门: 规则层报了 ≠ 告警出去了
        if review_down and not review_ready:
            v.append(_verdict(
                "warn", "复核服务没起（日志显示连不上）",
                "占座候选要送复核、**复核确认了才告警**; 拿不到结论时按 review.alert_on_failure "
                "兜底（config.test.yaml 里是 false ⇒ 不告警）。所以就算规则层报了占座, "
                "这轮也不会出现告警。", review_down[-1]))
        elif review_ready:
            v.append(_verdict("ok", "复核服务就绪（候选能被裁决）", "", review_ready[-1]))

    if push_off:
        v.append(_verdict(
            "info", "告警推送已禁用（只落库）",
            "这只影响 webhook 推送, **不影响**本页事件列表（本页读的是日志）。", push_off[-1]))

    if db_deg:
        v.append(_verdict(
            "warn", "数据库不可用 ⇒ 已降级写本地 db_fallback.csv",
            "落库链路是断的（不影响事件列表, 但别指望库里查得到）。", db_deg[-1]))

    # 「我改的那份到底是不是程序读的那份」
    # ⚠ 坑: 程序把「系统配置加载成功: <路径>」打到 **stdout**(ConfigParser.cpp 用 std::cout),
    #   日志文件里根本没有这行 ⇒ 原先只靠日志的这条判据**永远不触发**(实测面板里查无此条)。
    #   这里给两条来源: ①日志里若真有就按它(相对启动目录展开); ②否则退回文档约定的启动方式
    #   —— 从 RESULT_DIR(=build/) 启动、--config 用相对路径 ⇒ 程序读的是 <RESULT_DIR>/config/<同名>。
    raw = (_RE_CFG_LOADED.search(cfg_line[-1]).group(1) if cfg_line
           else os.path.join("config", os.path.basename(CONFIG_PATH)))
    prog_cfg = raw if os.path.isabs(raw) else os.path.join(RESULT_DIR, raw)
    same = _same_file_content(prog_cfg, CONFIG_PATH)
    v.append(_verdict(
        "info" if same else "warn",
        ("程序加载的配置与网页在编辑的是同一份：" if same
         else "程序加载的配置与网页在编辑的**不是同一份**：")
        + "程序= " + prog_cfg + " ／ 网页= " + CONFIG_PATH,
        "把程序启动目录下的那份展开成真实路径、与服务端**逐字节比对**（不再只比文件名, "
        "两份都叫 config.test.yaml 会骗过旧判据）。网页保存座位时会顺手同步 build/config "
        "镜像；两者内容不一致时, 你在网页上画的座位永远不会生效。",
        cfg_line[-1] if cfg_line else (lines[-1] if lines else expected)))

    other = _other_log()
    if other:
        v.append(_verdict(
            "warn", "仓库根还有一份日志 ⇒ 程序是从**仓库根**启动的",
            "网页读的是 " + expected + "。从仓库根启动时 video.source_path 的 ../test.mp4 "
            "必然打不开, 而且日志与 output.avi 都落在仓库根 ⇒ 请在 build/ 下启动。",
            other))

    return v


def _run_diagnosis(log_path, lines):
    """给 /api/events 附一份「为什么没有告警」的自证材料（全部可回溯到日志原文）。"""
    return {
        "log": log_path,
        "log_mtime": (time.strftime("%Y-%m-%d %H:%M:%S",
                                    time.localtime(os.path.getmtime(log_path)))
                      if log_path and os.path.isfile(log_path) else None),
        "config": _config_digest(),
        "verdicts": _diagnose(lines),
    }


def _mp4_cache(avi_path):
    """结果视频的 mp4 缓存路径（与源视频同目录）。"""
    stem = avi_path[:-4] if avi_path.lower().endswith(".avi") else avi_path
    return stem + ".web.mp4"


def _ensure_mp4(avi_path):
    """确保有浏览器能播的 mp4；返回 (mp4 路径, 错误信息)。

    output.avi 是 cv::VideoWriter 写的 MJPEG —— Chrome/Firefox **放不了**，
    所以这里用 ffmpeg 转一次 H.264(mp4) 并缓存；源视频更新后自动重转。
    """
    out = _mp4_cache(avi_path)
    if (os.path.isfile(out) and os.path.getsize(out) > 0
            and os.path.getmtime(out) >= os.path.getmtime(avi_path)):
        return out, None                       # 缓存还有效

    ffmpeg = shutil.which(FFMPEG_BIN)
    if not ffmpeg:
        return None, (f"系统里找不到 ffmpeg（FFMPEG_BIN={FFMPEG_BIN}），"
                      "无法把 MJPEG 的 AVI 转成网页能播的 MP4。"
                      "可以点下面的「下载原始 AVI」用本地播放器看。")
    cmd = [ffmpeg, "-y", "-i", avi_path,
           "-c:v", "libx264", "-preset", "veryfast", "-crf", "26",
           "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-an", out]
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=900)
    except subprocess.TimeoutExpired:
        return None, "ffmpeg 转码超过 15 分钟（视频太大或机器太慢）"
    if proc.returncode != 0 or not os.path.isfile(out):
        tail = proc.stderr.decode("utf-8", "replace")[-300:]
        return None, "ffmpeg 转码失败: " + tail
    return out, None


@app.route('/api/results')
def api_results():
    """流水线产物清单：结果视频 / 日志 / 降级 CSV。页面据此决定显示什么。"""
    avi = _safe_under(RESULT_DIR, RESULT_VIDEO)
    video = _file_info(avi, "带框结果视频（C++ 端 VideoWriter 写出的 AVI/MJPEG）")
    mp4_ready = False
    if avi and video.get("exists"):
        cache = _mp4_cache(avi)
        mp4_ready = (os.path.isfile(cache) and os.path.getsize(cache) > 0
                     and os.path.getmtime(cache) >= os.path.getmtime(avi))
    log = _find_log()
    csv = os.path.join(RESULT_DIR, "db_fallback.csv")
    return jsonify(
        ok=True,
        result_dir=RESULT_DIR,
        video=video,
        video_playable=mp4_ready,
        video_has_ffmpeg=bool(shutil.which(FFMPEG_BIN)),
        log=_file_info(log, "日志文件（占座/告警事件从这里抽）"),
        fallback_csv=_file_info(csv, "数据库不可用时的降级落盘"),
    )


@app.route('/api/result-video')
def api_result_video():
    """返回浏览器可播放的 mp4（必要时先 ffmpeg 转码；结果缓存复用）。"""
    avi = _safe_under(RESULT_DIR, request.args.get("name") or RESULT_VIDEO)
    if not avi or not os.path.isfile(avi):
        return jsonify(ok=False, error=f"结果视频不存在：{RESULT_DIR}/{RESULT_VIDEO}"), 404
    mp4, err = _ensure_mp4(avi)
    if err:
        return jsonify(ok=False, error=err), 503
    # conditional=True => 支持 HTTP Range，<video> 才能拖动进度条
    resp = send_file(mp4, mimetype="video/mp4", conditional=True)
    resp.headers["Cache-Control"] = "no-cache"      # 以 mtime 为准，避免播到旧缓存
    return resp


@app.route('/api/result-video-raw')
def api_result_video_raw():
    """原始 AVI（不计转码）：给本地播放器下载用。"""
    avi = _safe_under(RESULT_DIR, request.args.get("name") or RESULT_VIDEO)
    if not avi or not os.path.isfile(avi):
        return jsonify(ok=False, error="结果视频不存在"), 404
    return send_file(avi, mimetype="video/x-msvideo", as_attachment=True,
                     download_name=os.path.basename(avi), conditional=True)


# ---------------------- "哪个地方、哪个框"：场景示意 + 事件结构化 ----------------------
# 日志里写的是「座位 A-12: 检出物品 [laptop]…」，但看的人并不知道 A-12 在画面的哪儿。
# 这里补两件事：
#   1) /api/scene       —— 座位区多边形（从 config 的 occupancy.seats 读）+ 视频尺寸
#   2) /api/scene-frame —— 结果视频**第一帧**当底图，页面把座位框叠上去
# 于是"事件发生在哪"从一句文本变成画面上的一个高亮区域。

# 事件行的精确筛子。刻意**不**只是 grep "告警" —— 那会把
#   "告警去重: 已启用(…)" / "告警去重统计: allowed=0…" 这类**启动配置行与统计行**
#   也当成事件塞进列表（列表被噪声淹没，真事件反而看不见）。
#   [鉴权] 同理：只有"拒绝/失败"才是事件，"已关闭/已开启"只是启动时的配置陈述。
_EVENT_KEEP = re.compile(r"\[占座\]|告警推送失败|复核确认|\[鉴权\].*(?:拒绝|失败|不通过)")
# "未检出物品"是收尾统计里的一行(这一轮该座位最终没东西), 不是事件
_EVENT_SKIP = re.compile(r"统计\s*[:=]|已(启用|禁用)\s*[（(]|未检出物品")

# 日志行首时间戳(带毫秒) —— 事件要定位到**视频的哪一秒**, 靠它算
_TS_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)")
# 形如: 2026-10-05 09:15:07.216 [INFO ] [tid] [占座] 座位 A-12: ...
_EVENT_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})\S*\s+\[\w+\s*\]"
                       r"\s+\[[^\]]+\]\s+(.*)$")


def _parse_ts(line):
    """日志行首时间戳 → epoch 秒(带毫秒)。解不出返回 None。"""
    m = _TS_RE.match(line)
    if not m:
        return None
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try:
            return time.mktime(time.strptime(m.group(1), fmt))
        except ValueError:
            continue
    return None

# 座位名 / 物品 / 持续时长 / 最近见到人 / 投票（全部可选，缺了就是不给这个字段）
_RE_SEAT = re.compile(r"座位\s+(\S+?)\s*[:：]")
_RE_ITEMS = re.compile(r"检出物品\s*\[([^\]]*)\]")
_RE_DWELL = re.compile(r"已连续\s*(\d+)\s*秒")
_RE_PERSON = re.compile(r"人在座位上是\s*(\d+)\s*秒前")
_RE_VOTE = re.compile(r"近\s*(\d+)\s*帧中\s*(\d+)\s*帧")


def _parse_event(text):
    """把一行事件日志解析成结构化字段（解不出来就只留原文，不会丢信息）。"""
    ev = {"kind": "other"}
    if "[占座]" in text:
        ev["kind"] = "occupancy"
    elif "告警推送失败" in text:
        ev["kind"] = "alert_error"
    elif "[鉴权]" in text:
        ev["kind"] = "auth"

    m = _RE_SEAT.search(text)
    if m:
        ev["seat"] = m.group(1)
    m = _RE_ITEMS.search(text)
    if m:
        ev["items"] = [s.strip() for s in m.group(1).split(",") if s.strip()]
    m = _RE_DWELL.search(text)
    if m:
        ev["dwell_s"] = int(m.group(1))
    if "全程未检测到人" in text:
        ev["person_absent_s"] = None        # None = 全程没见过人
    else:
        m = _RE_PERSON.search(text)
        if m:
            ev["person_absent_s"] = int(m.group(1))
    m = _RE_VOTE.search(text)
    if m:
        ev["vote"] = f"{m.group(2)}/{m.group(1)}"
    return ev


def _load_seats():
    """从 config 读座位区。返回 (seats, hint)。

    文件位置/编码处理留在这里；**解析**在 seats_yaml.parse_seats()（纯文本层，
    与回写同一套正则 —— 读得出来的就一定写得回去，反之亦然）。
    seats = [{"name":…, "polygon":[[x,y],…], "kind":"rect"|"polygon",
              "rect":[x,y,w,h]?}]  —— polygon 一律展开成顶点，画图直接可用；
    kind/rect 只是把**原始写法**留个记号，P1 的"点选标定"回填时要保持商家原来怎么写的。
    """
    if not CONFIG_PATH or not os.path.isfile(CONFIG_PATH):
        return [], (f"没找到配置文件（CVINFER_CONFIG={CONFIG_PATH}），"
                    "无法在画面上标出座位位置")
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        return [], f"读配置失败: {e}"
    return seats_yaml.parse_seats(text)


# ------------------- 座位标定回写（网页点选 → occupancy.seats） -------------------
# 设计取舍: **不做 YAML 整体解析再 dump**。
# 配置文件是商家手写的现场记录, 里面全是"为什么把 zone 画在这儿"的注释;
# 用 YAML 库 round-trip 会把这些注释清掉 —— 那就等于毁了他的标定依据。
# 所以这里只在**文本层**把 occupancy.seats 这一个块换掉, 其余每一个字节
# (注释/空行/键顺序/行尾注释)原样保留。
# ⚠ 这套文本层逻辑(定位 seats 块/渲染/替换/结构自检)全在 **seats_yaml.py**：
#   它零第三方依赖, 所以那条"只换这一段"的约定能被 CI 直接守住
#   (web_gateway/test_seats_yaml.py)；本文件只负责 HTTP / 环境变量 / 起进程。
# 另外: "座位是否重叠/顶点是否为负"这类**判定口径**不在这里重写一遍 ——
# 落盘前交给 C++ 的 `--check-config`(见 _check_config), 保证与程序启动时同一口径。

CONFIG_BACKUP_SUFFIX = _env_str("CVINFER_CONFIG_BACKUP", ".bak")
CHECK_CONFIG_TIMEOUT = _env_int("CHECK_CONFIG_TIMEOUT", 60)


def _read_config_text():
    """读 CVINFER_CONFIG 原文。返回 (text, err)。"""
    if not CONFIG_PATH:
        return None, "没指定配置文件（CVINFER_CONFIG 为空）"
    if not os.path.isfile(CONFIG_PATH):
        return None, f"没找到配置文件：{CONFIG_PATH}"
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            return f.read(), None
    except OSError as e:
        return None, f"读配置失败: {e}"


def _check_config(path):
    """用 C++ 的 `--check-config` 验一遍候选配置。返回 (ok, msg)。

    ⚠ 这里是"口径单一来源"的落点：座位重叠率、顶点约束等规则**只**由
    ConfigParser::validate() 定义。Python 若不自己重写一遍，就永远不会有
    "网页说能存、程序却起不来"的分叉。二进制缺失时放行但明确告警。
    """
    if not (CVINFER_BIN and os.path.isfile(CVINFER_BIN) and os.access(CVINFER_BIN, os.X_OK)):
        return True, (f"未找到 C++ 程序（{CVINFER_BIN}），已跳过提交前校验 —— "
                      "先构建一次才能保证写回的配置一定加载得起来")
    try:
        p = subprocess.run([CVINFER_BIN, "--check-config", "--config", path],
                           capture_output=True, text=True, timeout=CHECK_CONFIG_TIMEOUT)
    except subprocess.TimeoutExpired:
        return False, f"C++ 配置校验超时（{CHECK_CONFIG_TIMEOUT}s）"
    except OSError as e:
        return False, f"起不来 C++ 配置校验: {e}"
    if p.returncode == 0:
        return True, None
    msgs = []
    for ln in (p.stderr or "").splitlines():
        m = re.match(r"^\[ConfigParser\]\s*[^:]*失败[::]\s*(.+)$", ln.strip())
        if m:
            msgs.append(m.group(1))
    if not msgs:
        tail = [l.strip() for l in (p.stderr or "").splitlines() if l.strip()]
        msgs = tail[-1:] or ["配置校验失败（C++ 未给出原因，可手工跑 "
                             f"{CVINFER_BIN} --check-config --config {path}）"]
    return False, "；".join(msgs)


_video_meta_cache = {}


def _video_meta(avi_path):
    """视频尺寸/帧数（OpenCV 只读元数据，不解码）。失败返回 {}。"""
    try:
        mtime = os.path.getmtime(avi_path)
    except OSError:
        return {}
    hit = _video_meta_cache.get(avi_path)
    if hit and hit[0] == mtime:
        return hit[1]
    cap = cv2.VideoCapture(avi_path)
    try:
        if not cap.isOpened():
            return {}
        meta = {
            "width": int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)),
            "height": int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT)),
            "frames": int(cap.get(cv2.CAP_PROP_FRAME_COUNT)),
            "fps": round(float(cap.get(cv2.CAP_PROP_FPS)), 3),
        }
    finally:
        cap.release()
    _video_meta_cache[avi_path] = (mtime, meta)
    return meta


@app.route('/api/scene')
def api_scene():
    """页面画"画面示意"要的全部东西：视频尺寸 + 座位区多边形 + 底图地址。"""
    avi = _safe_under(RESULT_DIR, RESULT_VIDEO)
    exists = bool(avi and os.path.isfile(avi))
    seats, hint = _load_seats()
    return jsonify(ok=True,
                   video_exists=exists,
                   size=_video_meta(avi) if exists else {},
                   config=CONFIG_PATH if os.path.isfile(CONFIG_PATH) else None,
                   seats=seats,
                   seat_hint=hint,
                   frame_url="/api/scene-frame" if exists else None)


_frame_cache = {}


@app.route('/api/scene-frame')
def api_scene_frame():
    """结果视频的第一帧（页面拿它当"画面示意"底图，再叠座位框）。"""
    avi = _safe_under(RESULT_DIR, RESULT_VIDEO)
    if not avi or not os.path.isfile(avi):
        return jsonify(ok=False, error="还没有结果视频（先跑一次 C++ 主程序）"), 404
    mtime = os.path.getmtime(avi)
    hit = _frame_cache.get(avi)
    data = hit[1] if (hit and hit[0] == mtime) else None
    if data is None:
        cap = cv2.VideoCapture(avi)
        try:
            ok, frame = cap.read()
        finally:
            cap.release()
        if not ok:
            return jsonify(ok=False, error="读视频首帧失败"), 500
        ok, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 88])
        if not ok:
            return jsonify(ok=False, error="首帧编码失败"), 500
        data = buf.tobytes()
        _frame_cache[avi] = (mtime, data)
    resp = Response(data, mimetype="image/jpeg")
    resp.headers["Cache-Control"] = "no-cache"
    return resp


# ---------------------- 座位标定探针：上传视频 → 在线热力图 ----------------------

def _extract_probe_json(stdout_text):
    """从探针 stdout 里捞出那行 JSON。

    为什么不直接 json.loads(整段)：探针跑模型时 OpenVINO/其它库偶尔会往 stdout 打日志。
    稳妥做法 = 逐行（从后往前）尝试解析，取第一个带 'cells' 的对象。
    """
    for line in reversed(stdout_text.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if isinstance(obj, dict) and "cells" in obj:
            return obj
    return None


def _run_seat_probe(video_path):
    """跑 C++ 探针（口径单一来源）。成功返回 (report, None)，失败 (None, (msg, code))。"""
    if not (CVINFER_BIN and os.path.isfile(CVINFER_BIN) and os.access(CVINFER_BIN, os.X_OK)):
        return None, (f"找不到 C++ 程序：{CVINFER_BIN}（先构建一次，或用 CVINFER_BIN 指定路径）", 500)
    cmd = [
        CVINFER_BIN,
        "--config", CONFIG_PATH,
        "--model-config", MODEL_CONFIG_PATH,
        "--seat-probe", video_path,
        "--seat-probe-json",
        "--seat-probe-stride", str(max(1, SEAT_PROBE_STRIDE)),
    ]
    try:
        proc = subprocess.run(cmd, cwd=_REPO_ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=SEAT_PROBE_TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, (f"探针超时（>{SEAT_PROBE_TIMEOUT}s）：调小 SEAT_PROBE_STRIDE 或换更短视频。", 504)
    except OSError as e:
        return None, (f"启动探针失败：{e}", 500)
    if proc.returncode != 0:
        tail = proc.stderr.decode("utf-8", "replace").strip().splitlines()[-3:]
        return None, ("探针执行失败：" + " | ".join(tail), 500)
    report = _extract_probe_json(proc.stdout.decode("utf-8", "replace"))
    if report is None:
        return None, ("探针没有输出可解析的 JSON（检查 --config / --model-config 指向的模型是否存在）", 500)
    return report, None


def _heat_bgr(ratio):
    """热力配色：冷=绿 → 热=红（BGR）。"""
    r = max(0.0, min(1.0, ratio))
    return (0, int(210 * (1.0 - r)), int(255 * r))


def _blend_rect(img, x, y, w, h, color, alpha):
    """在 img 上按 alpha 混一层纯色（只作用于相交区域，越界自动裁掉）。"""
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(img.shape[1], x + w), min(img.shape[0], y + h)
    if x1 <= x0 or y1 <= y0:
        return
    roi = img[y0:y1, x0:x1]
    overlay = np.empty_like(roi)
    overlay[:] = color
    cv2.addWeighted(overlay, alpha, roi, 1.0 - alpha, 0, roi)


def _render_probe_heatmap(frame, report):
    """把**已经算好的**格子命中数画到底图上 —— 只渲染，绝不重算归属。"""
    img = frame.copy()
    cell_w = int(report.get("cell_w") or 0)
    cell_h = int(report.get("cell_h") or 0)
    cols = int(report.get("cols") or 0)
    rows = int(report.get("rows") or 0)
    cells = report.get("cells") or []
    max_hit = max([int(c.get("item_hits", 0)) for c in cells] or [0])

    # 1) 网格线（细灰）：让"哪一格"一眼可读
    for c in range(1, cols):
        cv2.line(img, (c * cell_w, 0), (c * cell_w, img.shape[0]), (90, 90, 90), 1)
    for r in range(1, rows):
        cv2.line(img, (0, r * cell_h), (img.shape[1], r * cell_h), (90, 90, 90), 1)

    # 2) 热格：item 命中越多越红越实（只染有命中的格，避免一片色块看不出重点）
    for cell in cells:
        hits = int(cell.get("item_hits", 0))
        if hits <= 0:
            continue
        ratio = (hits / max_hit) if max_hit else 0.0
        x, y = int(cell["col"]) * cell_w, int(cell["row"]) * cell_h
        _blend_rect(img, x, y, cell_w, cell_h, _heat_bgr(ratio), 0.22 + 0.42 * ratio)
        p_hits = int(cell.get("person_hits", 0))
        txt = f"{hits}" + (f" / p{p_hits}" if p_hits else "")
        cv2.putText(img, txt, (x + 6, y + 24), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                    (255, 255, 255), 2)

    # 3) 建议 zone：亮青色粗框 + 角标（可直接照着填 occupancy.seats）
    zone = report.get("suggested_zone")
    if report.get("suggestion_valid") and isinstance(zone, dict):
        x, y = int(zone["x"]), int(zone["y"])
        w, h = int(zone["width"]), int(zone["height"])
        cv2.rectangle(img, (x, y), (x + w, y + h), (255, 255, 0), 3)
        (tw, th), _ = cv2.getTextSize("suggested zone", cv2.FONT_HERSHEY_SIMPLEX, 0.6, 2)
        ty = y - 8 if y - th - 8 > 0 else y + th + 10
        cv2.rectangle(img, (x, ty - th - 6), (x + tw + 12, ty + 6), (255, 255, 0), -1)
        cv2.putText(img, "suggested zone", (x + 6, ty), cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                    (0, 0, 0), 2)
    return img


@app.route('/api/seat-probe', methods=['POST'])
def api_seat_probe():
    """上传一段视频 → 跑 C++ 标定探针 → 返回热力图(dataURL) + 结构化数据。

    为什么值得有：occupancy.seats 是"每个现场都不一样"的静态标定，让商家手改 YAML 像素
    坐标几乎对不准。这里让他**看着图**指位置。口径仍由 C++ 决定（框底边中点落格），
    Python 只把已算好的格子画出来 ⇒ 热力图与线上判定同源，不会"图上这样、判定那样"。
    """
    if 'video' not in request.files:
        return jsonify(ok=False, error="未上传视频"), 400
    up = request.files['video']
    if not up.filename:
        return jsonify(ok=False, error="视频文件名为空"), 400

    suffix = os.path.splitext(up.filename)[1].lower() or ".mp4"
    fd, tmp = tempfile.mkstemp(prefix="seat_probe_", suffix=suffix)
    os.close(fd)
    try:
        up.save(tmp)
        report, err = _run_seat_probe(tmp)
        if err:
            return jsonify(ok=False, error=err[0]), err[1]

        # 底图 = 这段视频的一帧（探针跑的就是它，所以图上的位置就是判定的位置）
        cap = cv2.VideoCapture(tmp)
        try:
            ok_frame, frame = cap.read()
        finally:
            cap.release()

        heatmap_url = frame_url = ""
        warn = None
        if not ok_frame:
            warn = "视频抽帧失败：只有数据、没有图"
        else:
            ok_buf, buf = cv2.imencode(".jpg", _render_probe_heatmap(frame, report),
                                       [int(cv2.IMWRITE_JPEG_QUALITY), 88])
            if ok_buf:
                heatmap_url = ("data:image/jpeg;base64,"
                               + base64.b64encode(buf.tobytes()).decode("ascii"))
            ok_buf, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
            if ok_buf:
                frame_url = ("data:image/jpeg;base64,"
                             + base64.b64encode(buf.tobytes()).decode("ascii"))

        # report 里是 width/height/cols/rows/cell_w/cell_h/cells/suggestion_valid/
        # suggested_zone/label_top —— 前端热力图直接用，P1 画 zone 也吃这份坐标。
        # stride 一起回：底图是**首帧**、计数却是**整段视频**按 stride 抽样的 ⇒ 页面上那句
        # "人的落点比物品多"必须能说清它是怎么来的（否则商家会以为图和数对不上）。
        # item_labels 也一起回：前端要能区分"模型没认出来"与"认出来了但没算成物品"
        #   （实测：handbag 检出了 90 次, 但配置里写的是 "bag" ⇒ 规则层根本没把它当物品）。
        return jsonify(ok=True, video_name=up.filename, warn=warn,
                       stride=SEAT_PROBE_STRIDE,
                       item_labels=_config_digest().get("item_labels") or [],
                       config=CONFIG_PATH if os.path.isfile(CONFIG_PATH) else None,
                       heatmap=heatmap_url, frame=frame_url, **report)
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


@app.route('/api/seats')
def api_seats():
    """读配置里的座位区 —— 给「点选标定」当初始值（商家改现成的，别从零画）。"""
    seats, hint = _load_seats()
    return jsonify(ok=True, seats=seats, count=len(seats), hint=hint,
                   config=CONFIG_PATH if os.path.isfile(CONFIG_PATH) else None)


@app.route('/api/seats', methods=['POST'])
def api_seats_save():
    """把网页上点选的座位 zone 写回配置的 occupancy.seats。

    为什么需要它：occupancy.seats 是**每个现场都要重标**的静态常量，而"手改 YAML
    像素坐标"几乎必然对不准（上一版的热力图已经让人**看见**了位置，这一步让他**指**出来）。
    三条原则：
      1) **只动 seats 这一段**：其余字节（注释/空行/键顺序）原样保留 —— 那是商家的现场记录；
      2) **判定口径不复制**：重叠/顶点约束交给 C++ `--check-config` 验候选文件，
         验过了才落盘 ⇒ "网页说能存" 与 "程序真能启动" 永远一致；
      3) 落盘前**先备份**（<config>.bak，一层撤销），再原子替换。
    dry_run=true = 只校验不写入（前端"校验"按钮 / 后续 P2 即时反馈复用这条路径）。
    注意：写回后**需要重启 C++ 主程序**才生效（配置只在启动时读一次）。
    """
    body = request.get_json(silent=True) or {}
    seats = body.get("seats")
    dry = bool(body.get("dry_run"))
    try:
        width = int(body.get("width") or 0)
        height = int(body.get("height") or 0)
    except (TypeError, ValueError):
        return jsonify(ok=False, error="width/height 必须是整数"), 400

    issues = seats_yaml.validate_seats_payload(seats, width, height)
    if issues:
        return jsonify(ok=False, issues=issues), 400

    norm = [seats_yaml.norm_seat(s) for s in seats]
    text, err = _read_config_text()
    if err:
        return jsonify(ok=False, error=err), 500
    new_text, kept, err = seats_yaml.splice_seats(text, norm)
    if err:
        return jsonify(ok=False, error=err), 400
    preview = "".join(seats_yaml.render_seats_yaml(norm, 4))
    keep_note = (f"（保留了 seats 段原有的 {kept} 行说明注释）" if kept else "")

    # ---- 提交前校验：把**候选**配置写到临时文件, 让 C++ 用同一份口径验 ----
    tmpdir = os.path.dirname(os.path.abspath(CONFIG_PATH)) or "."
    fd, cand = tempfile.mkstemp(prefix=".config_seats_", suffix=".yaml", dir=tmpdir)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(new_text)
        checked, msg = _check_config(cand)
    finally:
        try:
            os.remove(cand)
        except OSError:
            pass
    if not checked:
        return jsonify(ok=False, issues=[msg], preview=preview, kept_comments=kept,
                       checked_by="c++", dry_run=dry), 400

    # 二进制缺失时 _check_config 会"放行 + 给告警"(见它的注释)。这条告警**不能吞掉**:
    # 页面上那句"校验通过"会被商家读成"C++ 认过了"。所以把"跳过"如实回给前端,
    # 让它改用橙色提示而不是绿字 —— P2 的即时反馈靠这个字段决定说什么。
    checked_by = "c++" if msg is None else "skipped"
    note = msg or None

    if dry:
        return jsonify(ok=True, dry_run=True, seats=norm, count=len(norm),
                       preview=preview, kept_comments=kept, checked_by=checked_by,
                       checked_note=note, config=CONFIG_PATH,
                       hint=("校验通过（未写入配置）" if note is None else
                             "没找到 C++ 程序，这一版没被真正校验（也未写入配置）") + keep_note)

    backup = CONFIG_PATH + CONFIG_BACKUP_SUFFIX
    try:
        shutil.copy2(CONFIG_PATH, backup)
        fd2, tmp2 = tempfile.mkstemp(prefix=".config_new_", dir=tmpdir)
        with os.fdopen(fd2, "w", encoding="utf-8") as f:
            f.write(new_text)
        os.chmod(tmp2, os.stat(CONFIG_PATH).st_mode)
        os.replace(tmp2, CONFIG_PATH)
    except OSError as e:
        return jsonify(ok=False, error=f"写配置失败: {e}"), 500

    # 顺手刷新 build/ 下的镜像: 程序 `cd build && … --config config/config.test.yaml`
    # 读的是那一份; 不同步 ⇒ 网页里画得再准也不生效（见 _sync_build_config_mirror 注释）。
    mirrored = _sync_build_config_mirror()

    return jsonify(ok=True, seats=norm, count=len(norm), preview=preview,
                   kept_comments=kept, checked_by=checked_by, checked_note=note,
                   config=CONFIG_PATH, backup=backup, mirror=mirrored,
                   hint="已写回配置（只改了 occupancy.seats）" + keep_note + "。"
                        + (f"已同步给程序按启动目录读到的那份：{mirrored}。" if mirrored else "")
                        + "要生效请重启 C++ 主程序 —— 配置只在启动时读一次。"
                        + ("" if note is None else f"⚠ {note}"))


@app.route('/api/event-frame')
def api_event_frame():
    """事件发生**那一刻**的画面（?t=<秒>，取自 /api/events 的 video_t）。

    这一帧是从 output.avi 里直接抽的 —— 也就是说**检测框已经画在画面上了**，
    不需要任何重建。这才是“事件证据”最直接的形式：既知道在哪，也知道当时框住了什么。
    """
    avi = _safe_under(RESULT_DIR, RESULT_VIDEO)
    if not avi or not os.path.isfile(avi):
        return jsonify(ok=False, error="还没有结果视频（先跑一次 C++ 主程序）"), 404
    try:
        t = max(0.0, float(request.args.get("t", 0)))
    except (TypeError, ValueError):
        t = 0.0

    cap = cv2.VideoCapture(avi)
    try:
        if not cap.isOpened():
            return jsonify(ok=False, error="打不开结果视频（可能正在被写入）"), 500
        fps = cap.get(cv2.CAP_PROP_FPS) or 10.0
        cap.set(cv2.CAP_PROP_POS_FRAMES, int(round(t * fps)))
        ok, frame = cap.read()
        if not ok:                        # 越界 ⇒ 退回首帧，别给用户一张空白
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            ok, frame = cap.read()
        if not ok:
            return jsonify(ok=False, error="读帧失败"), 500
    finally:
        cap.release()

    ok, buf = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 88])
    if not ok:
        return jsonify(ok=False, error="编码失败"), 500
    resp = Response(buf.tobytes(), mimetype="image/jpeg")
    resp.headers["Cache-Control"] = "no-cache"
    return resp


@app.route('/api/events')
def api_events():
    """从日志抽**真正的行为事件**（占座 / 告警失败 / 鉴权拒绝）并解析出关键字段。"""
    log = _find_log()
    if not log or not os.path.isfile(log):
        return jsonify(
            ok=True, source=None, events=[], hint=(
                "没找到日志文件 —— 这是**最常见**的原因：config 里 log.file 留空时日志只进"
                "控制台、不落盘。请把它设成 logs/cvinfer.log（或用环境变量 RESULT_LOG 指定）。"),
            diagnosis=_run_diagnosis(None, []),
        )
    events = []
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            lines = [ln.rstrip("\n") for ln in deque(f, maxlen=6000)]
    except OSError as e:
        return jsonify(ok=False, error=f"读日志失败: {e}"), 500

    # 「为什么一条告警都没有」：先按**最后一轮**抽一份自证材料（每条都带日志原文），
    # 再回到事件抽取 —— 两件事各用一份 lines, 互不干扰。
    diag = _run_diagnosis(log, _last_round(lines))

    # 只认**最后一轮**：output.avi 每次重跑都会被覆盖，前几轮的事件已经没有对应画面了。
    anchors = [i for i, ln in enumerate(lines) if "服务就绪" in ln]
    anchor_ts = _parse_ts(lines[anchors[-1]]) if anchors else None
    if anchors:
        lines = lines[anchors[-1]:]

    for line in lines:
        if not _EVENT_KEEP.search(line) or _EVENT_SKIP.search(line):
            continue
        m = _EVENT_RE.match(line)
        text = (m.group(2) if m else line).strip()
        ev = _parse_event(text)
        ev["time"] = m.group(1) if m else ""
        # 事件在**视频里的第几秒**：用「服务就绪」当锚点，把墙钟时间折算成帧号。
        #   前提是“处理速率 == 输出帧率”（文件源 target_fps 限速时成立，实测两次运行
        #   同一事件只差 0.1s）。若推理慢到跟不上、或换成非限速源，这里会漂 ——
        #   要根治就得让 C++ 在事件里带 frame_seq（那要改 C++）。
        ts = _parse_ts(line)
        if ts is not None and anchor_ts is not None:
            ev["video_t"] = round(max(0.0, ts - anchor_ts), 1)
        ev["text"] = text
        events.append(ev)
    return jsonify(ok=True, source=os.path.basename(log),
                   log_mtime=time.strftime("%Y-%m-%d %H:%M:%S",
                                           time.localtime(os.path.getmtime(log))),
                   count=len(events),
                   diagnosis=diag,
                   events=events[-120:])          # 最近 120 条足够看


# 5. 轻量状态：只探 TCP 端口（不触发推理、不依赖 Health RPC）
@app.route('/api/status')
def api_status():
    host, _, port_s = GRPC_SERVER.partition(':')
    host = host or "127.0.0.1"
    try:
        port = int(port_s)
    except ValueError:
        port = 50051
    reachable = False
    try:
        with socket.create_connection((host, port), timeout=1.0):
            reachable = True
    except OSError:
        reachable = False
    return jsonify(ok=True, server=GRPC_SERVER, reachable=reachable,
                   auth=bool(GRPC_AUTH_TOKEN), max_msg_mb=GRPC_MAX_MSG_MB)


if __name__ == '__main__':
    # 监听地址与端口来自环境变量（默认 0.0.0.0:8080）
    print(f"[web_gateway] gRPC -> {GRPC_SERVER} "
          f"(timeout={GRPC_TIMEOUT_MS}ms, max_msg={GRPC_MAX_MSG_MB}MB)")
    print(f"[web_gateway] 结果目录 -> {RESULT_DIR} "
          f"({'存在' if os.path.isdir(RESULT_DIR) else '不存在'})")
    if not shutil.which(FFMPEG_BIN):
        print("[web_gateway] 提示: 没找到 ffmpeg，结果视频无法在线播放（可下载 AVI）")
    print(f"[web_gateway] HTTP -> http://{WEB_HOST}:{WEB_PORT}")
    app.run(host=WEB_HOST, port=WEB_PORT, debug=False)
