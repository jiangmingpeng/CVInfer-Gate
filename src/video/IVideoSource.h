#pragma once
#include <opencv2/opencv.hpp>
#include <string>

class IVideoSource {
public:
    virtual ~IVideoSource() = default;
    // 打开视频源，返回是否成功
    virtual bool open(const std::string& source_path) = 0;
    // 读取一帧图像到 cv::Mat 中，返回是否成功（读到结尾或出错返回 false）
    virtual bool read(cv::Mat& frame) = 0;
    // 释放资源
    virtual void close() = 0;

    virtual int getWidth() const = 0;
    virtual int getHeight() const = 0;
};