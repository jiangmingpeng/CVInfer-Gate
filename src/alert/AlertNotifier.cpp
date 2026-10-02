#include "alert/AlertNotifier.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>

#include "utils/Logger.h"

namespace alert {

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string formatConfidence(float c) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", c);
    return std::string(buf);
}

} // namespace

AlertNotifier::~AlertNotifier() {
    stop();
}

bool AlertNotifier::init(const Config& cfg) {
    return init(cfg, [](const http::Request& req) { return http::request(req); });
}

bool AlertNotifier::init(const Config& cfg, Transport transport) {
    cfg_ = cfg;
    transport_ = std::move(transport);

    if (!cfg_.enabled) return false;
    std::string host, path;
    int port = 0;
    if (!http::parseUrl(cfg_.url, host, port, path)) {
        CVLOG_ERROR << "[告警推送] URL 非法(仅支持 http://host[:port]/path): " << cfg_.url;
        return false;
    }
    if (cfg_.max_queue == 0) cfg_.max_queue = 1;

    enabled_ = true;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stopping_ = false;
    }
    worker_ = std::thread([this] { workerLoop(); });
    return true;
}

bool AlertNotifier::push(const std::string& type, const std::string& description) {
    Alert a;
    a.type = type;
    a.description = description;
    return push(a);
}

bool AlertNotifier::push(const Alert& a) {
    if (!enabled_) return false;

    bool dropped_oldest = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (stopping_) return false;
        while (queue_.size() >= cfg_.max_queue) {
            queue_.pop_front(); // 与帧队列同策略: 丢旧帧保实时
            stats_.dropped++;
            dropped_oldest = true;
        }
        queue_.push_back(a);
        stats_.pushed++;
    }
    if (dropped_oldest) {
        static std::atomic<std::uint64_t> warn_count{0}; // 防日志刷屏: 每 100 次丢一条 WARN
        if (warn_count.fetch_add(1, std::memory_order_relaxed) % 100 == 0) {
            CVLOG_WARN << "[告警推送] 队列已满(" << cfg_.max_queue << "), 丢弃最旧告警"
                       << " —— 下游 webhook 可能不可用或过慢";
        }
    }
    cv_.notify_one();
    return true;
}

void AlertNotifier::stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!enabled_ || stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    enabled_ = false;
}

void AlertNotifier::workerLoop() {
    std::unique_lock<std::mutex> lock(mtx_);
    const auto drain_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.drain_timeout_ms);

    while (true) {
        cv_.wait_for(lock, std::chrono::milliseconds(200),
                     [this] { return stopping_ || !queue_.empty(); });

        while (!queue_.empty()) {
            // 停机排空有预算: 否则一个挂死的 webhook 会拖住整个退出流程
            if (stopping_ && std::chrono::steady_clock::now() > drain_deadline) {
                const std::size_t left = queue_.size();
                stats_.dropped += left;
                queue_.clear();
                CVLOG_WARN << "[告警推送] 退出排空超时, 放弃 " << left << " 条未发送告警";
                break;
            }

            Alert a = queue_.front();
            queue_.pop_front();
            if (a.ts_ms == 0) a.ts_ms = nowMs();
            lock.unlock();

            // 发送(带重试; 期间不持锁, 不阻塞 push)
            http::Request req;
            req.method = "POST";
            req.url = cfg_.url;
            req.timeout_ms = cfg_.timeout_ms;
            req.body = toJson(a);
            // 自定义头只在"名字与值都给全"时才发 —— 只给了名字不发空值头
            // (与 ConfigParser 的校验口径对称: 值非空才要求名字)
            if (!cfg_.header_name.empty() && !cfg_.header_value.empty()) {
                req.headers[cfg_.header_name] = cfg_.header_value;
            }

            const int attempts = cfg_.max_retries + 1;
            bool delivered = false;
            std::string last_error;
            for (int i = 0; i < attempts && !delivered; ++i) {
                if (i > 0) {
                    int backoff = cfg_.retry_backoff_ms * (1 << (i - 1));
                    if (backoff > 5000) backoff = 5000;
                    std::unique_lock<std::mutex> wait_lock(mtx_);
                    if (stopping_) {
                        // 停机中: **不干等退避**(否则退出被退避拖成秒级), 但下面仍会马上再试一次 ——
                        // 已入队的告警尽量送出去, 总时长由 drain_deadline 兜底。
                        backoff = 0;
                    } else {
                        cv_.wait_for(wait_lock, std::chrono::milliseconds(backoff),
                                     [this] { return stopping_; });
                    }
                    // 超出排空预算: 本条彻底放弃(外层会把它计入 dropped)
                    if (stopping_ && std::chrono::steady_clock::now() > drain_deadline) break;
                    stats_.retried++; // wait_lock 已持有 mtx_, 不要再上锁(会自锁)
                }
                const http::Response resp = transport_ ? transport_(req) : http::Response{};
                if (resp.ok && resp.status >= 200 && resp.status < 300) {
                    delivered = true;
                } else if (resp.ok) {
                    last_error = "HTTP " + std::to_string(resp.status);
                } else {
                    last_error = resp.error.empty() ? "未知错误" : resp.error;
                }
            }

            lock.lock();
            const std::int64_t ts = nowMs();
            if (delivered) {
                stats_.sent++;
                stats_.last_ok_ms = ts;
            } else {
                stats_.failed++;
                stats_.last_err_ms = ts;
                stats_.last_error = last_error;
            }
            if (delivered || stopping_) {
                CVLOG_DEBUG << "[告警推送] " << (delivered ? "已送达" : "放弃")
                            << (delivered ? "" : (": " + last_error));
            } else {
                CVLOG_WARN << "[告警推送] 失败(" << last_error << "): " << a.type;
            }
        }

        if (stopping_ && queue_.empty()) break;
    }
}

AlertNotifier::Stats AlertNotifier::stats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
}

std::string AlertNotifier::jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { // 控制字符 -> \u00XX
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c); // UTF-8 原样透传(JSON 允许)
                }
        }
    }
    return out;
}

std::string AlertNotifier::toJson(const Alert& a) {
    std::ostringstream oss;
    oss << "{\"source\":\"cvinfer-gate\""
        << ",\"alert_type\":\"" << jsonEscape(a.type) << "\""
        << ",\"description\":\"" << jsonEscape(a.description) << "\""
        << ",\"frame_seq\":" << a.frame_seq
        << ",\"label\":\"" << jsonEscape(a.label) << "\""
        << ",\"confidence\":" << formatConfidence(a.confidence)
        << ",\"track_id\":" << a.track_id
        << ",\"ts_ms\":" << a.ts_ms
        << "}";
    return oss.str();
}

} // namespace alert
