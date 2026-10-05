#pragma once

#include <algorithm>
#include <cmath>

#include <opencv2/opencv.hpp>

// RoiUtils (T16/T22: 目标 ROI 外扩与裁剪)
// 供 CascadeEngine(二级分类器复核, T16) 与 main(大模型复核, T22) 共用的
// 纯几何工具, 避免两处重复实现"外扩 + 裁剪到图像边界"。
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

// ---- 场景级 ROI [占座/复核场景扩展] --------------------------------------
// 起因: 送审大模型时若只裁"单个物体"(一本书 -> 107x135 特写), 模型看不到
//       桌面/座位/周围有没有人, 对"占座"这类**场景/关系**问题天然无法回答。
// 做法: 先按 padding 外扩, 再以**框中心**为基准放大 scale 倍(>=1), 最后保证
//       最小边长 min_side(像素; 0 = 不补), 全部裁剪到图像边界。
// 边界: scale <= 1 且 min_side <= 0 时**退化为 expandAndClamp**, 与原行为一致
//       => 老配置(不写这两个新键)行为完全不变。
inline cv::Rect expandAndClampContext(const cv::Rect& box, const cv::Size& size,
                                      float padding, float scale, int min_side) {
    const cv::Rect base = expandAndClamp(box, size, padding);
    if (base.width <= 0 || base.height <= 0) return cv::Rect();

    if (scale < 1.0f) scale = 1.0f;
    if (min_side < 0) min_side = 0;

    int w = base.width;
    int h = base.height;
    if (scale > 1.0f) {
        w = static_cast<int>(std::lround(static_cast<double>(w) * scale));
        h = static_cast<int>(std::lround(static_cast<double>(h) * scale));
    }
    if (min_side > 0) {
        w = std::max(w, min_side);
        h = std::max(h, min_side);
    }
    if (w == base.width && h == base.height) return base;

    const int cx = base.x + base.width / 2;
    const int cy = base.y + base.height / 2;
    int x1 = cx - w / 2;
    int y1 = cy - h / 2;
    int x2 = x1 + w;
    int y2 = y1 + h;
    // 贴近边界时向内推移(尽量保住"包含物体中心"这一语义)
    if (x1 < 0) {
        x2 -= x1;
        x1 = 0;
    }
    if (y1 < 0) {
        y2 -= y1;
        y1 = 0;
    }
    if (x2 > size.width) {
        x1 -= (x2 - size.width);
        x2 = size.width;
    }
    if (y2 > size.height) {
        y1 -= (y2 - size.height);
        y2 = size.height;
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 <= x1 || y2 <= y1) return cv::Rect();
    return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

// 裁剪"场景 ROI"(深拷贝); 无效返回空 Mat
inline cv::Mat cropContext(const cv::Mat& frame, const cv::Rect& box, float padding,
                           float scale, int min_side) {
    if (frame.empty()) return cv::Mat();
    const cv::Rect r = expandAndClampContext(box, frame.size(), padding, scale, min_side);
    if (r.width <= 0 || r.height <= 0) return cv::Mat();
    return frame(r).clone();
}

} // namespace roi_utils
