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

    // 源帧率(fps); 拿不到时返回 0(调用方需自行回退默认值)。
    // 输出视频的**容器帧率**需要它 —— 抽帧后应为 fps/frame_interval,
    // 否则回看会快放(实测 RTSP 场景里写出了 8 倍速的结果视频)。
    // 默认实现返回 0, 故已有实现(FakeVideoSource/测试抳件)不受影响。
    virtual double getFps() const { return 0.0; }
};