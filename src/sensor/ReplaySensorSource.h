#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "sensor/ISensorSource.h"

namespace sensor {

// ============================================================
// ReplaySensorSource (T24 骨架: 文件回放 / 合成)
// ------------------------------------------------------------
// 真实部署里雷达/红外各有 SDK/UDP/串口协议; 本类先实现**可离线验证**的两种
// backend, 让整条多模态链路在无硬件时也能端到端跑通、且可复现:
//
//   backend="file": 逐行回放文本文件, **每行 = 一次采样(单目标)**, 格式:
//         [timestamp_ms,]label,confidence[,distance_m[,x,y,w,h]]
//      - timestamp_ms 省略 => 用读取时刻(nowMs());
//      - 提供 timestamp_ms  => 按"相对首行时间戳"的节奏**限速**回放,
//        以贴近真实流速(否则整份文件会被瞬间读完、时间窗全部命中);
//      - '#' 开头与空行忽略;
//      - **多目标**写成同一时间戳的多行即可 —— SensorFusion::align 会按
//        时间窗把它们聚合到一起。
//   backend="stub": 无硬件时的合成源, 按 rate_hz 周期产出示例目标:
//      - 雷达: 无像素框, 只给 distance_m/azimuth_deg(走"标签关联"路径);
//      - 红外: 带一个缓动的热目标框(走"IoU 关联"路径)。
//      两条路径都被覆盖, 便于验证融合分支。
//
// 接入真实硬件: 继承本类并覆写 read()(从 UDP/串口取一帧 -> SensorTarget),
// 或在 open() 里建立连接; 限速/时间戳/生命周期语义沿用基类即可。
// ============================================================
class ReplaySensorSource : public ISensorSource {
public:
    ReplaySensorSource(std::string name, SensorKind kind)
        : name_(std::move(name)), kind_(kind) {}
    ~ReplaySensorSource() override;

    const std::string& name() const override { return name_; }
    SensorKind kind() const override { return kind_; }

    bool open(const SensorConfig& cfg) override;
    void close() override;                  // 幂等 + 打断限速中的 read()
    bool read(SensorSample& out) override;  // 按 backend_ 分支
    std::int64_t lastTimestampMs() const override { return last_ts_.load(); }

protected:
    // 派生类接入真实硬件时覆写 read() 即可; 下面两个是骨架实现
    bool readFromFile(SensorSample& out);
    bool readFromStub(SensorSample& out);

private:
    bool openFile(const SensorConfig& cfg);
    // 分段睡眠: 保证 close() 后 <=kSlice 毫秒内退出(否则 stop() 会卡住 join)
    void sleepInterruptible(std::int64_t ms) const;

    std::string name_;
    SensorKind kind_;
    std::string backend_ = "stub";
    std::vector<std::string> labels_;   // 标签白名单(空 = 不过滤)
    int rate_hz_ = 10;

    // ---- file backend ----
    std::ifstream file_;
    std::int64_t file_ts0_ = -1;   // 首行时间戳(限速基准)
    std::int64_t base_ms_ = 0;     // 回放起点(nowMs())

    // ---- stub / 无显式时间戳时的限速 ----
    std::uint64_t stub_tick_ = 0;
    std::int64_t next_tick_ms_ = 0;

    std::atomic<std::int64_t> last_ts_{0};
    std::atomic<bool> stop_{false};
};

}  // namespace sensor
