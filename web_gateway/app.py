import cv2
import numpy as np
import grpc
from flask import Flask, request, Response, render_template_string
from inference_pb2 import DetectRequest
from inference_pb2_grpc import DetectionServiceStub

app = Flask(__name__)

# 1. 连接 Docker 里映射出来的 C++ gRPC 服务
channel = grpc.insecure_channel('106.15.88.152:50051')
stub = DetectionServiceStub(channel)

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
        response = stub.Detect(request_grpc)
    except grpc.RpcError as e:
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
    # 监听 0.0.0.0 允许外部访问，端口 8080
    app.run(host='0.0.0.0', port=8080, debug=False)