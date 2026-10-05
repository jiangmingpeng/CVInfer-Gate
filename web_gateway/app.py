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
import os
import re
import shutil
import socket
import subprocess
import sys
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

# ---------------------- T10: 运行参数全部来自环境变量 ----------------------
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
# [T42] 主服务(50051)鉴权 token：非空则每次调用带 authorization: Bearer <token>
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
    """[T42] 主服务鉴权 metadata：token 非空才带。

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
            metadata=_grpc_metadata(),   # [T42] 空 token => ()，不带 metadata
        )
    except grpc.RpcError as e:
        # [T42] 鉴权失败最常见的原因：网关与服务端 env 不一致(或只配了一边)
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

# 只解析 occupancy.seats 里的 name + rect/polygon：够用且**不引入 yaml 依赖**
# （项目口径是"不轻易加依赖"）。解析失败就返回空列表，页面退化为"只显示事件文本"。
_SEAT_RE = re.compile(
    r"-\s*name\s*:\s*[\"']?(?P<name>[^\"'\n]+?)[\"']?\s*\n"
    r"\s*(?P<kind>rect|polygon)\s*:\s*(?P<val>[^\n]+)"
)


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
    """从 config 读座位区。返回 (seats, hint)；seats=[{"name":...,"polygon":[[x,y],…]}]。"""
    if not CONFIG_PATH or not os.path.isfile(CONFIG_PATH):
        return [], (f"没找到配置文件（CVINFER_CONFIG={CONFIG_PATH}），"
                    "无法在画面上标出座位位置")
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        return [], f"读配置失败: {e}"

    # 只取 occupancy: 段（到下一个顶格 key 为止），免得扫到别处的 rect/polygon
    m = re.search(r"^occupancy\s*:\s*$", text, re.M)
    if not m:
        return [], "配置里没有 occupancy 段（占座判定未启用？）"
    seg = text[m.end():]
    nxt = re.search(r"^\S", seg, re.M)     # 下一个顶格键 = 本段结束
    if nxt:
        seg = seg[:nxt.start()]

    seats = []
    for sm in _SEAT_RE.finditer(seg):
        name = sm.group("name").strip()
        val = sm.group("val").split("#")[0]  # 去掉行尾注释
        nums = [float(v) for v in re.findall(r"-?\d+(?:\.\d+)?", val)]
        if sm.group("kind") == "rect" and len(nums) >= 4:
            x, y, w, h = nums[:4]
            poly = [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]
        elif sm.group("kind") == "polygon" and len(nums) >= 6 and len(nums) % 2 == 0:
            poly = [[nums[i], nums[i + 1]] for i in range(0, len(nums), 2)]
        else:
            continue
        seats.append({"name": name, "polygon": poly})
    if not seats:
        return [], "配置里没解析出座位（occupancy.seats 为空？）"
    return seats, None


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
        )
    events = []
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            lines = [ln.rstrip("\n") for ln in deque(f, maxlen=6000)]
    except OSError as e:
        return jsonify(ok=False, error=f"读日志失败: {e}"), 500

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
