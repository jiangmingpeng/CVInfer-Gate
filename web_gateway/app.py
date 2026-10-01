import os
import cv2
import numpy as np
import grpc
from flask import Flask, request, Response, render_template_string
from inference_pb2 import DetectRequest
from inference_pb2_grpc import DetectionServiceStub

# ---------------------- T10: 运行参数全部来自环境变量 ----------------------
# 去除硬编码 IP：不再把 C++ 服务地址写死在源码里。
# 可选：若安装了 python-dotenv，则自动加载同目录 .env（本地开发方便）。
try:
    from dotenv import load_dotenv
    load_dotenv()
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

# 2. 一个极简的前端 HTML 页面（原生写在代码里，免去建模板文件）
HTML_PAGE = """
<!DOCTYPE html>
<html>
<head><title>CVInfer-Gate 网关</title></head>
<body style="text-align:center; font-family:sans-serif; margin-top:50px;">
    <h2>C++ AI 推理网关演示</h2>
    <form action="/detect" method="post" enctype="multipart/form-data">
        <input type="file" name="image" accept="image/*" required>
        <button type="submit" style="padding:5px 15px;">上传并检测</button>
    </form>
</body>
</html>
"""

# 3. 主页：显示上传表单
@app.route('/')
def index():
    return render_template_string(HTML_PAGE)

# 4. 核心接口：接收图片，调用 C++ gRPC，画框返回
@app.route('/detect', methods=['POST'])
def detect():
    if 'image' not in request.files:
        return "未上传图片", 400
    
    file = request.files['image']
    img_bytes = file.read()
    
    # 4.1 构造 gRPC 请求并调用 C++
    try:
        request_grpc = DetectRequest(image_data=img_bytes)
        response = stub.Detect(
            request_grpc,
            timeout=GRPC_TIMEOUT_MS / 1000.0,
            metadata=_grpc_metadata(),   # [T42] 空 token => ()，不带 metadata
        )
    except grpc.RpcError as e:
        # [T42] 鉴权失败最常见的原因：网关与服务端 env 不一致(或只配了一边)
        if e.code() == grpc.StatusCode.UNAUTHENTICATED:
            return ("C++ 服务调用失败: 鉴权不通过 —— 网关与服务端必须用同一个 "
                    "GRPC_AUTH_TOKEN（当前网关侧: %s）"
                    % ("已设置" if GRPC_AUTH_TOKEN else "未设置")), 500
        return f"C++ 服务调用失败: {e.details()}", 500
    
    if not response.success:
        return f"服务端推理失败: {response.message}", 500

    # 4.2 将收到的原始字节流解码为 OpenCV 图像
    nparr = np.frombuffer(img_bytes, np.uint8)
    img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    
    # 4.3 画框和文字
    for det in response.detections:
        cv2.rectangle(img, (det.x1, det.y1), (det.x2, det.y2), (0, 255, 0), 2)
        text = f"{det.label} {det.confidence:.2f}"
        cv2.putText(img, text, (det.x1, max(det.y1 - 10, 20)), 
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
    
    # 4.4 将画好框的图编码成 JPEG 返回给浏览器
    _, buffer = cv2.imencode('.jpg', img)
    return Response(buffer.tobytes(), mimetype='image/jpeg')

if __name__ == '__main__':
    # 监听地址与端口来自环境变量（默认 0.0.0.0:8080）
    print(f"[web_gateway] gRPC -> {GRPC_SERVER} "
          f"(timeout={GRPC_TIMEOUT_MS}ms, max_msg={GRPC_MAX_MSG_MB}MB)")
    print(f"[web_gateway] HTTP  -> {WEB_HOST}:{WEB_PORT}")
    app.run(host=WEB_HOST, port=WEB_PORT, debug=False)