#pragma once

#include <algorithm>
#include <cmath>

#include <opencv2/opencv.hpp>

// ============================================================
// RoiUtils (T16/T22: 目标 ROI 外扩与裁剪)
// ------------------------------------------------------------
// 供 CascadeEngine(二级分类器复核, T16) 与 main(大模型复核, T22) 共用的
// 纯几何工具, 避免两处重复实现"外扩 + 裁剪到图像边界"。
// ============================================================
namespace roi_utils {

// 将框按 padding(相对宽高比例)外扩并裁剪到图像边界; 无效返回空 Rect
inline cv::Rect expandAndClamp(const cv::Rect& box, const cv::Size& size, float padding) {
    if (padding < 0.0f) padding = 0.0f;
    const int dx = static_cast<int>(std::lround(box.width * padding));
    const int dy = static_cast<int>(std::lround(box.height * padding));

    int x1 = box.x - dx;
    int y1 = box.y - dy;
    int x2 = box.x + box.width + dx;
    int y2 = box.y + box.height + dy;

    x1 = std::max(0, x1);
    y1 = std::max(0, y1);
    x2 = std::min(size.width, x2);
    y2 = std::min(size.height, y2);

    if (x2 <= x1 || y2 <= y1) return cv::Rect();
    return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

// 裁剪目标 ROI(深拷贝, 便于跨线程/异步使用); 无效返回空 Mat
inline cv::Mat crop(const cv::Mat& frame, const cv::Rect& box, float padding) {
    if (frame.empty()) return cv::Mat();
    const cv::Rect r = expandAndClamp(box, frame.size(), padding);
    if (r.width <= 0 || r.height <= 0) return cv::Mat();
    return frame(r).clone();
}

}  // namespace roi_utils
