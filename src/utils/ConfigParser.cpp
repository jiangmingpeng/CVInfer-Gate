#include "utils/ConfigParser.h"

#include <cctype>
#include <cstdlib>
#include <regex>
#include <set>
#include <string>

namespace {

// 展开字符串中的环境变量引用 :
// ${VAR}           -> 取环境变量 VAR
// ${VAR:-default}  -> 取环境变量 VAR, 不存在则用 default
// 未定义且无默认值时保留原样(便于排查配置)
std::string expandEnv(const std::string& input) {
    static const std::regex re(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\})");
    std::string out;
    std::size_t last = 0;
    auto begin = std::sregex_iterator(input.begin(), input.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::smatch& m = *it;
        out.append(input, last, static_cast<std::size_t>(m.position()) - last);
        const char* env = std::getenv(m[1].str().c_str());
        if (env) {
            out.append(env); // 环境变量优先
        } else if (m[2].matched) {
            out.append(m[2].str()); // 回退到默认值
        } else {
            out.append(m.str()); // 保留原样
        }
        last = static_cast<std::size_t>(m.position() + m.length());
    }
    out.append(input, last, std::string::npos);
    return out;
}

// 读取字符串节点并展开环境变量; 节点缺失时返回默认值
std::string readStr(const YAML::Node& node, const std::string& key, const std::string& def) {
    if (node[key]) return expandEnv(node[key].as<std::string>(def));
    return def;
}

// 小写化(枚举类配置项容错: LATENCY/Throughput 均可)
std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 解析新格式 models: 列表中的单个模型条目
void parseModelEntry(const YAML::Node& n, ModelConfig& mc) {
    mc.name = n["name"].as<std::string>(mc.name);
    mc.role = n["role"].as<std::string>(mc.role);
    mc.model_xml_path = n["xml_path"].as<std::string>("");
    mc.model_bin_path = n["bin_path"].as<std::string>("");
    mc.labels_path = n["labels_path"].as<std::string>("");
    mc.input_width = n["input_width"].as<int>(640);
    mc.input_height = n["input_height"].as<int>(640);
    // 阈值: 既支持平铺 conf/nms, 也支持嵌套 thresholds:{conf,nms}
    if (n["conf"]) mc.conf_threshold = n["conf"].as<float>(mc.conf_threshold);
    else if (n["thresholds"]) mc.conf_threshold = n["thresholds"]["conf"].as<float>(mc.conf_threshold);
    if (n["nms"]) mc.nms_threshold = n["nms"].as<float>(mc.nms_threshold);
    else if (n["thresholds"]) mc.nms_threshold = n["thresholds"]["nms"].as<float>(mc.nms_threshold);
    mc.pool_size = n["pool_size"].as<int>(0);
    mc.acquire_timeout_ms = n["acquire_timeout_ms"].as<int>(mc.acquire_timeout_ms);
    // 推理性能旋钮(可选; 缺省=改造前行为)
    mc.device      = n["device"].as<std::string>(mc.device);
    mc.perf_mode   = toLower(n["performance_mode"].as<std::string>(mc.perf_mode));
    mc.num_threads = n["num_threads"].as<int>(mc.num_threads);
}

// 解析旧格式 (model: + thresholds:), 保持向后兼容
void parseLegacyModel(const YAML::Node& root, ModelConfig& mc) {
    if (root["model"]) {
        const YAML::Node m = root["model"];
        mc.model_xml_path = m["xml_path"].as<std::string>("");
        mc.model_bin_path = m["bin_path"].as<std::string>("");
        mc.labels_path = m["labels_path"].as<std::string>("");
        mc.input_width = m["input_width"].as<int>(640);
        mc.input_height = m["input_height"].as<int>(640);
        // 性能旋钮(旧格式放在 model: 下, 缺省即现状)
        mc.device      = m["device"].as<std::string>(mc.device);
        mc.perf_mode   = toLower(m["performance_mode"].as<std::string>(mc.perf_mode));
        mc.num_threads = m["num_threads"].as<int>(mc.num_threads);
    }
    if (root["thresholds"]) {
        mc.conf_threshold = root["thresholds"]["conf"].as<float>(0.25f);
        mc.nms_threshold = root["thresholds"]["nms"].as<float>(0.45f);
    }
    mc.name = "yolov8_detector";
    mc.role = "detector";
    mc.pool_size = 0;
}

} // namespace

bool ConfigParser::loadAppConfig(const std::string& filepath) {
    try {
        YAML::Node config = YAML::LoadFile(filepath);

        // app: 日志
        if (config["app"]) {
            app_config_.log.level = readStr(config["app"], "log_level", app_config_.log.level);
            app_config_.log.file  = readStr(config["app"], "log_file",  app_config_.log.file);
            // 日志文件轮转
            app_config_.log.max_size_mb =
                config["app"]["log_max_size_mb"].as<int>(app_config_.log.max_size_mb);
            app_config_.log.keep_files =
                config["app"]["log_keep_files"].as<int>(app_config_.log.keep_files);
        }

        // video
        if (config["video"]) {
            const YAML::Node v = config["video"];
            app_config_.video.source_type = readStr(v, "source_type", app_config_.video.source_type);
            app_config_.video.source_path = readStr(v, "source_path", app_config_.video.source_path);
            app_config_.video.target_fps  = v["target_fps"].as<int>(app_config_.video.target_fps);
            app_config_.video.frame_interval = v["frame_interval"].as<int>(app_config_.video.frame_interval);
            // queue 帧队列防爆配置
            if (v["queue"]) {
                app_config_.video.queue.max_size =
                    v["queue"]["max_size"].as<std::size_t>(app_config_.video.queue.max_size);
                app_config_.video.queue.policy =
                    readStr(v["queue"], "policy", app_config_.video.queue.policy);
            }
        }

        // pipeline
        if (config["pipeline"]) {
            app_config_.pipeline.worker_threads =
                config["pipeline"]["worker_threads"].as<int>(app_config_.pipeline.worker_threads);
        }

        // database
        if (config["database"]) {
            const YAML::Node d = config["database"];
            app_config_.database.host     = readStr(d, "host", app_config_.database.host);
            app_config_.database.user     = readStr(d, "user", app_config_.database.user);
            app_config_.database.password = readStr(d, "password", app_config_.database.password);
            app_config_.database.dbname   = readStr(d, "dbname", app_config_.database.dbname);
            // 以下为新增项
            app_config_.database.pool_size         = d["pool_size"].as<int>(app_config_.database.pool_size);
            app_config_.database.batch_size        = d["batch_size"].as<int>(app_config_.database.batch_size);
            app_config_.database.flush_interval_ms = d["flush_interval_ms"].as<int>(app_config_.database.flush_interval_ms);
            app_config_.database.max_retries       = d["max_retries"].as<int>(app_config_.database.max_retries);
            // 降级 / 重试策略
            app_config_.database.fallback_path = readStr(d, "fallback_path", app_config_.database.fallback_path);
            app_config_.database.reconnect_interval_ms =
                d["reconnect_interval_ms"].as<int>(app_config_.database.reconnect_interval_ms);
            app_config_.database.reconnect_after_writes =
                d["reconnect_after_writes"].as<int>(app_config_.database.reconnect_after_writes);
        }

        // grpc
        if (config["grpc"]) {
            const YAML::Node g = config["grpc"];
            app_config_.grpc.port = g["port"].as<int>(app_config_.grpc.port);
            // 以下为新增项
            app_config_.grpc.timeout_ms          = g["timeout_ms"].as<int>(app_config_.grpc.timeout_ms);
            app_config_.grpc.max_message_size_mb = g["max_message_size_mb"].as<int>(app_config_.grpc.max_message_size_mb);
            app_config_.grpc.worker_threads      = g["worker_threads"].as<int>(app_config_.grpc.worker_threads);
            app_config_.grpc.keepalive_time_ms   = g["keepalive_time_ms"].as<int>(app_config_.grpc.keepalive_time_ms);
            // 走 readStr => 支持 ${GRPC_AUTH_TOKEN:-} 环境变量展开(同 database.password)
            app_config_.grpc.auth_token          = readStr(g, "auth_token", app_config_.grpc.auth_token);
        }

        // metrics 指标端点(Prometheus)
        if (config["metrics"]) {
            const YAML::Node m = config["metrics"];
            app_config_.metrics.enabled = m["enabled"].as<bool>(app_config_.metrics.enabled);
            app_config_.metrics.bind    = readStr(m, "bind", app_config_.metrics.bind);
            app_config_.metrics.port    = m["port"].as<int>(app_config_.metrics.port);
        }

        // cascade
        if (config["cascade"]) {
            const YAML::Node c = config["cascade"];
            auto& ccfg = app_config_.cascade;
            ccfg.enabled   = c["enabled"].as<bool>(ccfg.enabled);
            ccfg.primary   = readStr(c, "primary", ccfg.primary);
            ccfg.secondary = readStr(c, "secondary", ccfg.secondary);
            ccfg.roi_padding    = c["roi_padding"].as<float>(ccfg.roi_padding);
            ccfg.accept_label   = readStr(c, "accept_label", ccfg.accept_label);
            ccfg.accept_conf    = c["accept_conf"].as<float>(ccfg.accept_conf);
            ccfg.drop_rejected  = c["drop_rejected"].as<bool>(ccfg.drop_rejected);
            ccfg.boost_on_confirm = c["boost_on_confirm"].as<bool>(ccfg.boost_on_confirm);

            // 触发规则(嵌套 trigger: {labels, min_conf, max_conf})
            if (c["trigger"]) {
                const YAML::Node t = c["trigger"];
                if (t["labels"] && t["labels"].IsSequence()) {
                    ccfg.trigger_labels.clear();
                    for (const auto& l : t["labels"]) {
                        ccfg.trigger_labels.push_back(l.as<std::string>());
                    }
                }
                ccfg.min_conf = t["min_conf"].as<float>(ccfg.min_conf);
                ccfg.max_conf = t["max_conf"].as<float>(ccfg.max_conf);
            }
        }

        // review
        if (config["review"]) {
            const YAML::Node r = config["review"];
            auto& rcfg = app_config_.review;
            rcfg.enabled          = r["enabled"].as<bool>(rcfg.enabled);
            rcfg.endpoint         = readStr(r, "endpoint", rcfg.endpoint);
            rcfg.timeout_ms       = r["timeout_ms"].as<int>(rcfg.timeout_ms);
            rcfg.queue_size       = r["queue_size"].as<std::size_t>(rcfg.queue_size);
            rcfg.worker_threads   = r["worker_threads"].as<int>(rcfg.worker_threads);
            rcfg.roi_padding      = r["roi_padding"].as<float>(rcfg.roi_padding);
            rcfg.prompt           = readStr(r, "prompt", rcfg.prompt);
            rcfg.alert_type       = readStr(r, "alert_type", rcfg.alert_type);
            rcfg.alert_on_failure = r["alert_on_failure"].as<bool>(rcfg.alert_on_failure);
            // 接入真实 VLM 服务端(传输/鉴权/探活)
            rcfg.max_message_size_mb = r["max_message_size_mb"].as<int>(rcfg.max_message_size_mb);
            rcfg.keepalive_time_ms   = r["keepalive_time_ms"].as<int>(rcfg.keepalive_time_ms);
            rcfg.auth_token          = readStr(r, "auth_token", rcfg.auth_token);
            rcfg.health_check        = r["health_check"].as<bool>(rcfg.health_check);

            // 触发规则(嵌套 trigger: {labels, min_conf, max_conf})
            if (r["trigger"]) {
                const YAML::Node t = r["trigger"];
                if (t["labels"] && t["labels"].IsSequence()) {
                    rcfg.trigger_labels.clear();
                    for (const auto& l : t["labels"]) {
                        rcfg.trigger_labels.push_back(l.as<std::string>());
                    }
                }
                rcfg.min_conf = t["min_conf"].as<float>(rcfg.min_conf);
                rcfg.max_conf = t["max_conf"].as<float>(rcfg.max_conf);
            }
        }

        // sensors 非视频传感器列表
        if (config["sensors"] && config["sensors"].IsSequence()) {
            app_config_.sensors.clear();
            for (const auto& node : config["sensors"]) {
                SensorConfig sc;
                sc.kind    = readStr(node, "kind", sc.kind);
                sc.name    = readStr(node, "name", sc.name);
                sc.backend = readStr(node, "backend", sc.backend);
                sc.path    = readStr(node, "path", sc.path);
                sc.rate_hz = node["rate_hz"].as<int>(sc.rate_hz);
                if (node["labels"] && node["labels"].IsSequence()) {
                    sc.labels.clear();
                    for (const auto& l : node["labels"]) {
                        sc.labels.push_back(l.as<std::string>());
                    }
                }
                if (sc.name.empty()) sc.name = sc.kind; // 未命名则用 kind 兜底
                app_config_.sensors.push_back(std::move(sc));
            }
        }

        // fusion 多模态决策级融合
        if (config["fusion"]) {
            const YAML::Node f = config["fusion"];
            auto& fc = app_config_.fusion;
            fc.enabled            = f["enabled"].as<bool>(fc.enabled);
            fc.level              = readStr(f, "level", fc.level);
            fc.time_tolerance_ms  = f["time_tolerance_ms"].as<int>(fc.time_tolerance_ms);
            fc.match_iou          = f["match_iou"].as<float>(fc.match_iou);
            fc.sensor_weight      = f["sensor_weight"].as<float>(fc.sensor_weight);
            fc.emit_sensor_only   = f["emit_sensor_only"].as<bool>(fc.emit_sensor_only);
            fc.adopt_sensor_label = f["adopt_sensor_label"].as<bool>(fc.adopt_sensor_label);
            fc.buffer_capacity    = f["buffer_capacity"].as<std::size_t>(fc.buffer_capacity);
        }

        // alert 告警去重
        if (config["alert"]) {
            const YAML::Node a = config["alert"];
            if (a["dedup"]) {
                const YAML::Node d = a["dedup"];
                auto& dc = app_config_.alert.dedup;
                dc.enabled     = d["enabled"].as<bool>(dc.enabled);
                dc.iou         = d["iou"].as<float>(dc.iou);
                dc.cooldown_ms = d["cooldown_ms"].as<int>(dc.cooldown_ms);
                dc.max_entries = d["max_entries"].as<int>(dc.max_entries);
            }
            // 告警推送(webhook): 推送是"通知", 落库才是"账" —— 推送失败不影响落库
            if (a["push"]) {
                const YAML::Node p = a["push"];
                auto& pc = app_config_.alert.push;
                pc.enabled          = p["enabled"].as<bool>(pc.enabled);
                pc.url              = readStr(p, "url", pc.url);
                pc.timeout_ms       = p["timeout_ms"].as<int>(pc.timeout_ms);
                pc.max_retries      = p["max_retries"].as<int>(pc.max_retries);
                pc.retry_backoff_ms = p["retry_backoff_ms"].as<int>(pc.retry_backoff_ms);
                pc.max_queue        = p["max_queue"].as<std::size_t>(pc.max_queue);
                pc.drain_timeout_ms = p["drain_timeout_ms"].as<int>(pc.drain_timeout_ms);
                pc.header_name      = readStr(p, "header_name", pc.header_name);
                pc.header_value     = readStr(p, "header_value", pc.header_value);
            }
        }

        // tracking 目标跟踪
        if (config["tracking"]) {
            const YAML::Node tr = config["tracking"];
            auto& tc = app_config_.tracking;
            tc.enabled     = tr["enabled"].as<bool>(tc.enabled);
            tc.iou         = tr["iou"].as<float>(tc.iou);
            tc.dist_factor = tr["dist_factor"].as<float>(tc.dist_factor);
            tc.max_age_ms  = tr["max_age_ms"].as<int>(tc.max_age_ms);
            tc.min_hits    = tr["min_hits"].as<int>(tc.min_hits);
            tc.max_tracks  = tr["max_tracks"].as<int>(tc.max_tracks);
        }

        std::cout << "[ConfigParser] 系统配置加载成功: " << filepath << std::endl;
        // [改进] 加载后立即校验, 提前暴露配置错误
        return validate();
    } catch (const YAML::Exception& e) {
        std::cerr << "[ConfigParser] 加载系统配置失败: " << e.what() << std::endl;
        return false;
    }
}

// 配置合法性校验
bool ConfigParser::validate() const {
    bool ok = true;
    auto fail = [&ok](const std::string& msg) {
        std::cerr << "[ConfigParser] 配置校验失败: " << msg << std::endl;
        ok = false;
    };

    // 日志
    static const std::set<std::string> kLevels = {"trace", "debug", "info", "warn", "error"};
    if (kLevels.find(app_config_.log.level) == kLevels.end())
        fail("app.log_level 非法(应为 trace/debug/info/warn/error): " + app_config_.log.level);

    // 视频
    if (app_config_.video.source_type != "file" && app_config_.video.source_type != "rtsp")
        fail("video.source_type 必须为 file 或 rtsp");
    if (app_config_.video.source_path.empty())
        fail("video.source_path 不能为空");
    if (app_config_.video.frame_interval < 1)
        fail("video.frame_interval 必须 >= 1");
    if (app_config_.video.target_fps < 0)
        fail("video.target_fps 不能为负");
    if (app_config_.video.queue.max_size < 1)
        fail("video.queue.max_size 必须 >= 1");
    if (app_config_.video.queue.policy != "drop_oldest" && app_config_.video.queue.policy != "block")
        fail("video.queue.policy 必须为 drop_oldest 或 block");
    // 交叉校验 source_type 与 source_path 前缀(高频手误: 类型忘了改 -> 静默走错视频源)
    {
        const std::string& sp = app_config_.video.source_path;
        const bool looks_rtsp = (sp.rfind("rtsp://", 0) == 0);
        if (app_config_.video.source_type == "file" && looks_rtsp)
            fail("video.source_type=file 但 source_path 是 RTSP 地址, 应改为 source_type: rtsp: " + sp);
        if (app_config_.video.source_type == "rtsp" && !looks_rtsp)
            fail("video.source_type=rtsp 但 source_path 不是 rtsp:// 开头: " + sp);
    }

    // 流水线
    if (app_config_.pipeline.worker_threads < 1)
        fail("pipeline.worker_threads 必须 >= 1");

    // 数据库
    if (app_config_.database.pool_size < 1)
        fail("database.pool_size 必须 >= 1");
    if (app_config_.database.batch_size < 1)
        fail("database.batch_size 必须 >= 1");
    if (app_config_.database.flush_interval_ms < 1)
        fail("database.flush_interval_ms 必须 >= 1");
    if (app_config_.database.max_retries < 0)
        fail("database.max_retries 不能为负");
    if (app_config_.database.reconnect_interval_ms < 1)
        fail("database.reconnect_interval_ms 必须 >= 1");
    if (app_config_.database.reconnect_after_writes < 1)
        fail("database.reconnect_after_writes 必须 >= 1");

    // gRPC
    if (app_config_.grpc.port <= 0 || app_config_.grpc.port > 65535)
        fail("grpc.port 必须位于 1..65535");
    if (app_config_.grpc.timeout_ms < 0)
        fail("grpc.timeout_ms 不能为负");
    if (app_config_.grpc.max_message_size_mb < 1)
        fail("grpc.max_message_size_mb 必须 >= 1");

    // 级联
    const auto& cs = app_config_.cascade;
    if (cs.min_conf < 0.0f || cs.min_conf > 1.0f)
        fail("cascade.trigger.min_conf 必须在 0..1");
    if (cs.max_conf < 0.0f || cs.max_conf > 1.0f)
        fail("cascade.trigger.max_conf 必须在 0..1");
    if (cs.min_conf > cs.max_conf)
        fail("cascade.trigger.min_conf 不能大于 max_conf");
    if (cs.roi_padding < 0.0f)
        fail("cascade.roi_padding 不能为负");
    if (cs.accept_conf < 0.0f || cs.accept_conf > 1.0f)
        fail("cascade.accept_conf 必须在 0..1");

    // 大模型复核
    const auto& rv = app_config_.review;
    if (rv.timeout_ms < 1)
        fail("review.timeout_ms 必须 >= 1");
    if (rv.queue_size < 1)
        fail("review.queue_size 必须 >= 1");
    if (rv.worker_threads < 1)
        fail("review.worker_threads 必须 >= 1");
    if (rv.min_conf < 0.0f || rv.min_conf > 1.0f)
        fail("review.trigger.min_conf 必须在 0..1");
    if (rv.max_conf < 0.0f || rv.max_conf > 1.0f)
        fail("review.trigger.max_conf 必须在 0..1");
    if (rv.min_conf > rv.max_conf)
        fail("review.trigger.min_conf 不能大于 max_conf");
    if (rv.roi_padding < 0.0f)
        fail("review.roi_padding 不能为负");
    if (rv.enabled && rv.endpoint.empty())
        fail("review.enabled=true 时 review.endpoint 不能为空");
    if (rv.max_message_size_mb < 1)
        fail("review.max_message_size_mb 必须 >= 1");
    if (rv.keepalive_time_ms < 0)
        fail("review.keepalive_time_ms 不能为负");

    // 传感器 / 融合
    const auto& fc = app_config_.fusion;
    if (fc.level != "decision")
        fail("fusion.level 目前仅支持 decision(决策级融合): " + fc.level);
    if (fc.time_tolerance_ms < 1)
        fail("fusion.time_tolerance_ms 必须 >= 1");
    if (fc.match_iou < 0.0f || fc.match_iou > 1.0f)
        fail("fusion.match_iou 必须在 0..1");
    if (fc.sensor_weight < 0.0f || fc.sensor_weight > 1.0f)
        fail("fusion.sensor_weight 必须在 0..1");
    if (fc.buffer_capacity < 1)
        fail("fusion.buffer_capacity 必须 >= 1");
    {
        static const std::set<std::string> kSensorKinds = {"video", "radar", "infrared"};
        static const std::set<std::string> kBackends    = {"file", "stub"};
        std::set<std::string> names;
        for (const auto& s : app_config_.sensors) {
            if (kSensorKinds.find(s.kind) == kSensorKinds.end())
                fail("sensors[].kind 非法(应为 video/radar/infrared): " + s.kind);
            // 视频由 video: 段负责, 不应在 sensors: 里重复声明(否则两处时间基不一致)
            if (s.kind == "video")
                fail("sensors[" + s.name + "] 的 kind=video 非法: 视频请用 video: 段声明");
            if (kBackends.find(s.backend) == kBackends.end())
                fail("sensors[" + s.name + "].backend 非法(应为 file/stub): " + s.backend);
            if (s.backend == "file" && s.path.empty())
                fail("sensors[" + s.name + "].backend=file 时 path 不能为空");
            if (s.rate_hz < 1)
                fail("sensors[" + s.name + "].rate_hz 必须 >= 1");
            if (!names.insert(s.name).second)
                fail("传感器名重复: " + s.name);
        }
        if (fc.enabled && app_config_.sensors.empty())
            fail("fusion.enabled=true 但未配置任何 sensors:");
    }

    // 告警去重
    const auto& dd = app_config_.alert.dedup;
    if (dd.iou < 0.0f || dd.iou > 1.0f)
        fail("alert.dedup.iou 必须在 0..1");
    if (dd.cooldown_ms < 0)
        fail("alert.dedup.cooldown_ms 不能为负");
    if (dd.max_entries < 1)
        fail("alert.dedup.max_entries 必须 >= 1");

    // 日志轮转
    if (app_config_.log.max_size_mb < 0)
        fail("app.log_max_size_mb 不能为负(0 = 不轮转)");
    if (app_config_.log.keep_files < 0)
        fail("app.log_keep_files 不能为负(0 = 只保留当前文件)");

    // 指标端点
    if (app_config_.metrics.port <= 0 || app_config_.metrics.port > 65535)
        fail("metrics.port 必须位于 1..65535");
    if (app_config_.metrics.port == app_config_.grpc.port)
        fail("metrics.port 不能与 grpc.port 相同: " + std::to_string(app_config_.grpc.port));
    if (app_config_.metrics.enabled && app_config_.metrics.bind.empty())
        fail("metrics.enabled=true 时 metrics.bind 不能为空(建议 127.0.0.1 或 0.0.0.0)");

    // 告警推送
    const auto& ap = app_config_.alert.push;
    if (ap.timeout_ms < 1)
        fail("alert.push.timeout_ms 必须 >= 1");
    if (ap.max_retries < 0)
        fail("alert.push.max_retries 不能为负");
    if (ap.retry_backoff_ms < 0)
        fail("alert.push.retry_backoff_ms 不能为负");
    if (ap.max_queue < 1)
        fail("alert.push.max_queue 必须 >= 1");
    if (ap.drain_timeout_ms < 0)
        fail("alert.push.drain_timeout_ms 不能为负");
    if (ap.enabled && ap.url.empty())
        fail("alert.push.enabled=true 时 alert.push.url 不能为空");
    if (ap.enabled && ap.url.rfind("http://", 0) != 0)
        fail("alert.push.url 目前仅支持 http:// (无 TLS; 公网请用内网转发/侧车): " + ap.url);
    // 自定义头: "给了名字但值暂时为空" 是常见且合法的组合(等环境变量注入) => 视为不发该头;
    // 反过来 "给了值却没给名字" 一定是写错了(发了也白发) => 拦下。
    if (!ap.header_value.empty() && ap.header_name.empty())
        fail("alert.push.header_value 非空时必须同时给 header_name");

    // 目标跟踪
    const auto& tc = app_config_.tracking;
    if (tc.iou < 0.0f || tc.iou > 1.0f)
        fail("tracking.iou 必须在 0..1");
    if (tc.dist_factor < 0.0f)
        fail("tracking.dist_factor 不能为负(0 = 关闭距离兜底)");
    if (tc.max_age_ms < 0)
        fail("tracking.max_age_ms 不能为负");
    if (tc.min_hits < 1)
        fail("tracking.min_hits 必须 >= 1");
    if (tc.max_tracks < 1)
        fail("tracking.max_tracks 必须 >= 1");

    return ok;
}

bool ConfigParser::loadModelConfig(const std::string& filepath) {
    try {
        YAML::Node config = YAML::LoadFile(filepath);
        model_configs_.clear();

        // 新格式: models: [ {name, role, xml_path, ...}, ... ]
        if (config["models"] && config["models"].IsSequence() && config["models"].size() > 0) {
            for (const auto& node : config["models"]) {
                ModelConfig mc;
                parseModelEntry(node, mc);
                model_configs_.push_back(std::move(mc));
            }
        } else {
            // 兼容旧的单模型格式 (model: + thresholds:)
            ModelConfig mc;
            parseLegacyModel(config, mc);
            model_configs_.push_back(std::move(mc));
        }

        model_config_ = model_configs_.front(); // 主模型 = 首个

        std::cout << "[ConfigParser] 模型配置加载成功: " << filepath
                  << " (模型数=" << model_configs_.size() << ")" << std::endl;
        return validateModelConfigs();
    } catch (const YAML::Exception& e) {
        std::cerr << "[ConfigParser] 加载模型配置失败: " << e.what() << std::endl;
        return false;
    }
}

// 模型配置合法性校验
bool ConfigParser::validateModelConfigs() const {
    bool ok = true;
    auto fail = [&ok](const std::string& msg) {
        std::cerr << "[ConfigParser] 模型配置校验失败: " << msg << std::endl;
        ok = false;
    };

    if (model_configs_.empty()) {
        fail("未配置任何模型 (models 为空)");
        return false;
    }

    std::set<std::string> names;
    for (const auto& m : model_configs_) {
        if (m.model_xml_path.empty())
            fail("模型 " + m.name + " 的 xml_path 不能为空");
        if (m.role != "detector" && m.role != "classifier" && m.role != "reviewer")
            fail("模型 " + m.name + " 的 role 非法(应为 detector/classifier/reviewer): " + m.role);
        // reviewer 已废弃: 大模型复核不走本地模型, 而在 config.yaml 的 review: 段配置。
        if (m.role == "reviewer")
            fail("模型 " + m.name + " 的 role=reviewer 已废弃: 大模型复核请改用 config.yaml 的 review: 段(T20-T22), 不要放入 models:");
        if (m.conf_threshold < 0.0f || m.conf_threshold > 1.0f)
            fail("模型 " + m.name + " 的 conf 阈值必须在 0..1");
        if (m.nms_threshold < 0.0f || m.nms_threshold > 1.0f)
            fail("模型 " + m.name + " 的 nms 阈值必须在 0..1");
        if (!names.insert(m.name).second)
            fail("模型名重复: " + m.name);
        // 性能旋钮校验
        if (m.device.empty())
            fail("模型 " + m.name + " 的 device 不能为空(可用 AUTO/CPU/GPU/NPU)");
        if (m.perf_mode != "" && m.perf_mode != "latency" && m.perf_mode != "throughput")
            fail("模型 " + m.name + " 的 performance_mode 非法(应为 latency/throughput 或留空): " + m.perf_mode);
        if (m.num_threads < 0)
            fail("模型 " + m.name + " 的 num_threads 不能为负(0 = 用 OpenVINO 默认)");
    }
    return ok;
}