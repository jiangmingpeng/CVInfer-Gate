#include "sensor/ReplaySensorSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "utils/Logger.h"

namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(trim(cur));
    return out;
}

// 整串都是十进制数字(允许前导 '-' / '+') —— 用于判定首列是否为时间戳
bool isInteger(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

bool labelAllowed(const std::vector<std::string>& allowed, const std::string& label) {
    if (allowed.empty()) return true;   // 未配置白名单 = 全放行
    return std::find(allowed.begin(), allowed.end(), label) != allowed.end();
}

}  // namespace

namespace sensor {

ReplaySensorSource::~ReplaySensorSource() {
    close();
}

bool ReplaySensorSource::open(const SensorConfig& cfg) {
    close();                 // 幂等重置(会把 stop_ 置位)
    stop_.store(false);      // 重新开启

    if (!cfg.name.empty()) name_ = cfg.name;
    backend_ = cfg.backend.empty() ? "stub" : cfg.backend;
    labels_ = cfg.labels;
    rate_hz_ = cfg.rate_hz > 0 ? cfg.rate_hz : 10;
    last_ts_.store(0);
    stub_tick_ = 0;
    next_tick_ms_ = nowMs();   // 立即产出第一个采样
    base_ms_ = nowMs();
    file_ts0_ = -1;

    if (backend_ == "file" && !openFile(cfg)) return false;

    CVLOG_INFO << "[Sensor:" << name_ << "] open: kind=" << sensorKindToString(kind_)
               << ", backend=" << backend_ << ", rate=" << rate_hz_ << "Hz"
               << (backend_ == "file" ? (", path=" + cfg.path) : "");
    return true;
}

bool ReplaySensorSource::openFile(const SensorConfig& cfg) {
    if (file_.is_open()) file_.close();
    file_.clear();
    file_.open(cfg.path);
    if (!file_.is_open()) {
        CVLOG_ERROR << "[Sensor:" << name_ << "] 无法打开回放文件: " << cfg.path;
        return false;
    }
    return true;
}

void ReplaySensorSource::close() {
    // 先置位再关文件: read() 的限速睡眠会在 <=10ms 内看到并退出,
    // 这样 MultiSensorPipeline::stop() 的 join 不会被拖住。
    stop_.store(true);
    if (file_.is_open()) file_.close();
}

void ReplaySensorSource::sleepInterruptible(std::int64_t ms) const {
    constexpr std::int64_t kSlice = 10;   // 10ms 粒度: 足够省 CPU, 又能及时响应 close()
    std::int64_t left = ms;
    while (left > 0 && !stop_.load()) {
        const std::int64_t chunk = std::min(kSlice, left);
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        left -= chunk;
    }
}

bool ReplaySensorSource::read(SensorSample& out) {
    if (stop_.load()) return false;
    return backend_ == "file" ? readFromFile(out) : readFromStub(out);
}

bool ReplaySensorSource::readFromStub(SensorSample& out) {
    // 限速: 距下次该产出还有多久就等多久(可被 close() 打断)
    const std::int64_t period_ms = 1000 / (rate_hz_ > 0 ? rate_hz_ : 10);
    const std::int64_t now = nowMs();
    if (next_tick_ms_ > now) {
        sleepInterruptible(next_tick_ms_ - now);
        if (stop_.load()) return false;
    }
    next_tick_ms_ = nowMs() + period_ms;

    // 确定性缓动(不用随机数 -> 可复现): 相位由 tick 计数推进
    const double phase = static_cast<double>(stub_tick_++) * 0.2;
    const double s = std::sin(phase);

    out = SensorSample{};                 // 复位(含 frame/targets)
    out.kind = kind_;
    out.timestamp_ms = nowMs();

    SensorTarget t;
    t.class_id = 0;
    t.label = labels_.empty() ? std::string("person") : labels_.front();
    t.confidence = static_cast<float>(0.6 + 0.3 * s);                 // 0.3..0.9
    if (kind_ == SensorKind::Radar) {
        // 雷达: 无像素框 -> 融合时走"标签关联"分支
        t.distance_m = static_cast<float>(5.0 + 3.0 * s);             // 2..8 m
        t.azimuth_deg = 10.0 * s;
    } else {
        // 红外: 带框 -> 融合时走"IoU 关联"分支
        t.distance_m = static_cast<float>(6.0 - 1.0 * s);
        t.box = cv::Rect2f(100.0f + 40.0f * static_cast<float>(s), 120.0f, 120.0f, 240.0f);
    }
    out.targets.push_back(std::move(t));

    last_ts_.store(out.timestamp_ms);
    return true;
}

bool ReplaySensorSource::readFromFile(SensorSample& out) {
    std::string line;
    while (std::getline(file_, line)) {          // 跳过空行/注释行
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        const std::vector<std::string> cols = split(t, ',');
        if (cols.size() < 2) continue;           // 至少需要 label,confidence

        // ---- 解析: [timestamp_ms,]label,confidence[,distance_m[,x,y,w,h]] ----
        std::size_t i = 0;
        std::int64_t ts = 0;
        bool explicit_ts = false;
        if (isInteger(cols[0])) {                // 首列是整数 => 时间戳
            ts = static_cast<std::int64_t>(std::strtoll(cols[0].c_str(), nullptr, 10));
            explicit_ts = true;
            i = 1;
        }
        if (cols.size() < i + 2) continue;       // label,confidence 缺一不可

        SensorTarget tg;
        tg.class_id = 0;
        tg.label = cols[i++];
        tg.confidence = static_cast<float>(std::atof(cols[i++].c_str()));
        if (i < cols.size() && !cols[i].empty()) {
            tg.distance_m = static_cast<float>(std::atof(cols[i++].c_str()));
        } else if (i < cols.size()) {
            ++i;                                 // 占位空字段
        }
        if (cols.size() >= i + 4) {              // x,y,w,h
            const float x = static_cast<float>(std::atof(cols[i + 0].c_str()));
            const float y = static_cast<float>(std::atof(cols[i + 1].c_str()));
            const float w = static_cast<float>(std::atof(cols[i + 2].c_str()));
            const float h = static_cast<float>(std::atof(cols[i + 3].c_str()));
            tg.box = cv::Rect2f(x, y, w, h);
        }
        if (!labelAllowed(labels_, tg.label)) continue;

        // ---- 时间戳 + 限速 ----
        const std::int64_t now = nowMs();
        if (explicit_ts) {
            if (file_ts0_ < 0) file_ts0_ = ts;   // 首行定基准
            const std::int64_t target = base_ms_ + (ts - file_ts0_);
            if (target > now) {
                sleepInterruptible(target - now);
                if (stop_.load()) return false;
            }
            // [T27 修正] 对外时间戳必须与其它模态同源(nowMs = steady_clock):
            //   ISensorSource.h 的约定就是"时间戳一律用 nowMs()"; 而回放文件的 ts
            //   通常是"相对首行"的小数值(如 1000000), 与 nowMs() 差几个数量级,
            //   若原样输出, MultiSensorPipeline 以 nowMs() 为锚点对齐时会全部落在
            //   时间窗之外(aligned=0 -> 融合静默失效)。限速已按源文件相对间隔完成,
            //   故这里取"读取时刻"; 源 ts 仅保留在调试日志中便于追溯。
            out.timestamp_ms = nowMs();
            CVLOG_DEBUG << "[Sensor:" << name_ << "] file 采样: src_ts=" << ts
                        << ", ts=" << out.timestamp_ms;
        } else {
            // 无显式时间戳: 按 rate_hz 限速, 时间戳取读取时刻
            const std::int64_t period_ms = 1000 / (rate_hz_ > 0 ? rate_hz_ : 10);
            if (next_tick_ms_ > now) {
                sleepInterruptible(next_tick_ms_ - now);
                if (stop_.load()) return false;
            }
            next_tick_ms_ = nowMs() + period_ms;
            out.timestamp_ms = nowMs();
        }

        out.kind = kind_;
        out.frame = cv::Mat();
        out.targets.clear();
        out.targets.push_back(std::move(tg));

        last_ts_.store(out.timestamp_ms);
        return true;
    }
    return false;   // EOF: 回放结束
}

}  // namespace sensor
