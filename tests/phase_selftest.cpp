// tests/phase_selftest.cpp
// Phase A~D 阶段自检 (T27)
// 目的: 不依赖 模型/传感器/复核服务/数据库, 也不需要改动 config/config.yaml,
// 用"测试内注入的假实现"驱动每个阶段的接缝, 一次跑完即可看到
// Phase A 抽象层 / Phase B 级联灰区 / Phase C 异步复核 / Phase D 融合
// 的完整行为与统计, 并长期作为回归测试。
//
// 原理: Phase A~D 的分层都是"面向接口"的, 因此每一层都能被替换:
// IDetector / IClassifier  -> FakeDetector / FakeClassifier   (Phase A/B)
// IReviewService           -> FakeReviewer                    (Phase C)
// SensorFusion             -> 纯函数, 直接喂合成样本            (Phase D)
// ISensorSource            -> ReplaySensorSource(backend=stub) (Phase D)
//
// 构建/运行: cmake --build . --target phase_selftest && ./phase_selftest

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "utils/ConfigParser.h"
#include "utils/Logger.h"
#include "inference/IModel.h"
#include "inference/CascadeEngine.h"
#include "review/IReviewService.h"
#include "review/ReviewScheduler.h"
#include "sensor/ISensorSource.h"
#include "sensor/ReplaySensorSource.h"
#include "fusion/SensorFusion.h"
#include "pipeline/MultiSensorPipeline.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
        std::cout << "  [ OK ] " << what << "\n";
    } else {
        ++g_fail;
        std::cout << "  [FAIL] " << what << "\n";
    }
}

void section(const std::string& title) {
    std::cout << "\n========== " << title << " ==========\n";
}

void note(const std::string& s) {
    std::cout << "  [NOTE] " << s << "\n";
}

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

DetectionResult mkDet(int class_id, const std::string& label, float conf, const cv::Rect& box) {
    DetectionResult d;
    d.class_id = class_id;
    d.label = label;
    d.confidence = conf;
    d.box = box;
    return d;
}

// Phase A/B 用的假实现: 替代 YoloDetector / BehaviorClassifier
class FakeDetector : public IDetector {
public:
    std::vector<DetectionResult> results; // 预置"主模型输出"
    DetectStatus status = DetectStatus::Ok;
    int calls = 0;

    bool init(const ModelConfig&) override { return true; }
    const std::string& name() const override {
        static const std::string n = "fake_detector";
        return n;
    }
    ModelRole role() const override { return ModelRole::Detector; }

    DetectStatus detect(const cv::Mat&, std::vector<DetectionResult>& out) override {
        ++calls;
        if (status != DetectStatus::Ok) return status;
        out = results; // 拷贝, 保持确定性
        return DetectStatus::Ok;
    }
};

class FakeClassifier : public IClassifier {
public:
    DetectStatus status = DetectStatus::Ok;
    Classification cls; // status==Ok 时返回
    int calls = 0;
    std::string last_roi;

    bool init(const ModelConfig&) override { return true; }
    const std::string& name() const override {
        static const std::string n = "fake_classifier";
        return n;
    }
    ModelRole role() const override { return ModelRole::Classifier; }

    DetectStatus classify(const cv::Mat& roi, Classification& out) override {
        ++calls;
        last_roi = std::to_string(roi.cols) + "x" + std::to_string(roi.rows);
        if (status != DetectStatus::Ok) return status;
        out = cls;
        return DetectStatus::Ok;
    }
};

// Phase C 用的假实现: 替代 GrpcLlmReviewer(不需要服务端)
// 按 frame_seq % 4 确定性产出四种结果, 便于断言:
// 0 -> Ok + 确认   1 -> Ok + 否决   2 -> 服务不可达   3 -> 超时
class FakeReviewer : public IReviewService {
public:
    std::atomic<int> calls{0};

    const std::string& name() const override {
        static const std::string n = "fake_reviewer";
        return n;
    }

    ReviewStatus review(const ReviewRequest& req, ReviewResult& out) override {
        ++calls;
        out = ReviewResult{};
        switch (req.frame_seq % 4) {
            case 0:
                out.confirmed = true;
                out.label = "no_helmet";
                out.confidence = 0.88f;
                out.reason = "scripted:confirmed";
                return ReviewStatus::Ok;
            case 1:
                out.confirmed = false;
                out.label = "with_helmet";
                out.confidence = 0.93f;
                out.reason = "scripted:rejected";
                return ReviewStatus::Ok;
            case 2:
                return ReviewStatus::Unavailable;
            default:
                std::this_thread::sleep_for(std::chrono::milliseconds(30)); // 模拟慢服务
                return ReviewStatus::Timeout;
        }
    }
};

// 构造所有 Phase B 场景共用的主模型输出
std::vector<DetectionResult> greyZoneResults() {
    return {
        mkDet(0, "person", 0.52f, cv::Rect(100, 100, 100, 200)), // 灰区内 -> 触发复核
        mkDet(0, "person", 0.97f, cv::Rect(400, 100, 100, 200)), // 高于上界 -> 不复核
        mkDet(2, "car",    0.60f, cv::Rect(600, 300, 200, 150)), // 类别不命中 -> 不复核
    };
}

// Phase A: 模型抽象层 (T12-T15)
void phaseA() {
    section("Phase A: 模型抽象层 (T12-T15)");

    auto det = std::make_shared<FakeDetector>();
    det->results = greyZoneResults();

    check(det->role() == ModelRole::Detector, "IModel 契约: role() == Detector");
    check(std::string(modelRoleToString(det->role())) == "detector",
          "role 字符串映射: detector");
    check(!det->name().empty(), "IModel 契约: name() 非空");

    const cv::Mat frame = cv::Mat::zeros(720, 1280, CV_8UC3);
    std::vector<DetectionResult> out;
    check(det->detect(frame, out) == DetectStatus::Ok, "IDetector::detect() 返回 Ok");
    check(out.size() == 3, "主模型输出 3 个目标 (2 person + 1 car)");
    check(out[0].fused == false && near(out[0].vision_confidence, -1.0f),
          "融合字段默认值: fused=false, vision_confidence=-1 (未融合)");
    check(out[0].reviewed == false && out[0].sub_class_id == -1,
          "级联字段默认值: reviewed=false, sub_class_id=-1 (未复核)");
    note("真实链路是同一接缝的另一实现(YoloDetector + ModelPoolManager)。");
    note("请用主程序日志验证 Phase A: '模型配置加载成功: ... (模型数=N)' + '使用检测器: ...'。");
}

// Phase B: 级联灰区复核 (T16-T19)
void phaseB() {
    section("Phase B: 级联主筛 + 灰区二级复核 (T16-T19)");

    CascadeConfig cfg;
    cfg.trigger_labels = {"person"};
    cfg.min_conf = 0.40f; // 灰区下界(含)
    cfg.max_conf = 0.90f; // 灰区上界(不含)
    cfg.roi_padding = 0.10f;
    cfg.accept_label = "with_helmet";
    cfg.accept_conf = 0.50f;
    cfg.drop_rejected = true;
    cfg.boost_on_confirm = false;

    auto primary = std::make_shared<FakeDetector>();
    primary->results = greyZoneResults();
    auto secondary = std::make_shared<FakeClassifier>();

    CascadeEngine cascade(primary, secondary, cfg);
    ModelConfig mc;
    mc.name = "cascade_selftest";
    cascade.init(mc);

    const cv::Mat frame = cv::Mat::zeros(720, 1280, CV_8UC3);

    // B0: 灰区判定 + ROI 计算 (public 接口, 可单测)
    check(cascade.isGrayZone(primary->results[0]), "灰区判定: person 0.52 ∈ [0.40,0.90) -> 触发");
    check(!cascade.isGrayZone(primary->results[1]), "灰区判定: person 0.97 >= 0.90 -> 不触发");
    check(!cascade.isGrayZone(primary->results[2]), "灰区判定: car 0.60 类别不命中 -> 不触发");
    check(!cascade.isGrayZone(mkDet(0, "person", 0.35f, cv::Rect(10, 10, 50, 50))),
          "灰区判定: person 0.35 < 0.40 (太弱, 不浪费二级算力) -> 不触发");

    const cv::Rect roi = cascade.roiFor(cv::Rect(100, 100, 100, 200), frame.size());
    check(roi.width > 0 && roi.height > 0 && roi.x <= 100 && roi.y <= 100 &&
              roi.x + roi.width <= frame.cols && roi.y + roi.height <= frame.rows,
          "ROI 按 padding 外扩并裁剪到图像内: " + std::to_string(roi.x) + "," +
              std::to_string(roi.y) + " " + std::to_string(roi.width) + "x" +
              std::to_string(roi.height));

    // B1: 二级"确认" -> 保留 + 回写 sub_*
    secondary->status = DetectStatus::Ok;
    secondary->cls = Classification{1, 0.91f, "with_helmet"};
    std::vector<DetectionResult> out1;
    check(cascade.detect(frame, out1) == DetectStatus::Ok, "级联 detect() 返回 Ok");
    check(out1.size() == 3, "确认: 目标全部保留 (3 个)");
    {
        bool ok = false;
        for (const auto& d : out1) {
            if (near(d.confidence, 0.52f)) {
                ok = d.reviewed && d.sub_label == "with_helmet" && near(d.sub_confidence, 0.91f);
            }
        }
        check(ok, "确认: 灰区目标带 reviewed=true / sub_label=with_helmet / sub_conf=0.91");
    }
    check(secondary->calls == 1, "只对 1 个灰区目标做了二级推理(非灰区不浪费算力)");
    check(primary->calls == 1, "主模型只被调用 1 次(每个目标仅复核 1 次)");
    {
        const auto s = cascade.stats();
        check(s.primary == 3 && s.triggered == 1 && s.confirmed == 1 &&
                  s.rejected == 0 && s.skipped == 0,
              "确认: 统计 primary=3 triggered=1 confirmed=1 rejected=0 skipped=0");
    }

    // B2: 二级"否决" + drop_rejected=true -> 丢弃
    secondary->cls = Classification{1, 0.12f, "no_helmet"};
    std::vector<DetectionResult> out2;
    cascade.detect(frame, out2);
    check(out2.size() == 2, "否决: 灰区目标被丢弃 (3 -> 2)");
    {
        bool gone = true;
        for (const auto& d : out2) if (near(d.confidence, 0.52f)) gone = false;
        check(gone, "否决: 被丢弃的正是那个 0.52 的 person");
    }
    {
        const auto s = cascade.stats();
        check(s.triggered == 2 && s.confirmed == 1 && s.rejected == 1 && s.skipped == 0,
              "否决: 累计统计 triggered=2 confirmed=1 rejected=1 skipped=0");
    }

    // B3: 二级"繁忙/失败" -> 降级保留(绝不误杀)
    secondary->status = DetectStatus::Busy;
    std::vector<DetectionResult> out3;
    cascade.detect(frame, out3);
    check(out3.size() == 3, "降级: 二级不可用时保留主结果 (3 个)");
    {
        const auto s = cascade.stats();
        check(s.triggered == 3 && s.skipped == 1 && s.rejected == 1,
              "降级: 累计统计 triggered=3 skipped=1 rejected=1 (confirmed 不变=1)");
    }

    // B4: 没有二级分类器 -> 退化为单模型直通(这正是真实工程里最常见的情况)
    std::shared_ptr<IClassifier> none;
    CascadeEngine degraded(primary, none, cfg);
    std::vector<DetectionResult> out4;
    degraded.detect(frame, out4);
    {
        const auto s = degraded.stats();
        check(out4.size() == 3 && s.triggered == 0,
              "退化: secondary=nullptr -> 单模型直通(triggered=0, 与 YoloDetector 行为一致)");
    }
    note("模型库里没有 role=classifier 的模型时, buildCascade 就是这种退化结果 —— 日志会打");
    note("'级联构建失败, 回退单模型检测器。'。这也是你当前跑出来'单模型模式'的原因之一。");
}

// Phase C: 大模型异步复核 (T20-T22)
void phaseC() {
    section("Phase C: 大模型异步复核 (T20-T22)");

    // C1: alert_on_failure = true (拿不到复核结论时兜底告警; **否决仍不告警**)
    {
        ReviewConfig cfg;
        cfg.enabled = true;
        cfg.queue_size = 64;
        cfg.worker_threads = 2;
        cfg.timeout_ms = 1000;
        cfg.alert_on_failure = true;

        auto svc = std::make_shared<FakeReviewer>();
        ReviewScheduler sched;
        std::mutex mtx;
        std::vector<ReviewOutcome> got;

        const bool ok = sched.init(cfg, svc,
                                   [&](const ReviewOutcome& o) {
                                       std::lock_guard<std::mutex> lk(mtx);
                                       got.push_back(o);
                                   });
        check(ok && sched.enabled(), "ReviewScheduler 初始化成功 + enabled()==true");

        int submitted = 0;
        for (std::uint64_t seq = 0; seq < 10; ++seq) {
            ReviewRequest job;
            job.frame_seq = seq;
            job.roi = cv::Mat::zeros(64, 64, CV_8UC3); // 非空 ROI(GrpcLlmReviewer 拒收空 ROI)
            job.label = "person";
            job.confidence = 0.72f;
            if (sched.submit(std::move(job))) ++submitted;
        }
        check(submitted == 10, "10 个复核任务全部入队(submit 非阻塞)");

        sched.stop(); // join 全部复核线程 => 回调必然已全部执行完
        const ReviewScheduler::Stats st = sched.stats();
        check(st.submitted == 10 && st.reviewed == 10, "统计: submitted=10 reviewed=10");
        check(st.confirmed == 3 && st.rejected == 3,
              "统计: confirmed=3 rejected=3 (frame_seq%4==0/1)");
        check(st.unavailable == 2 && st.timeout == 2,
              "统计: unavailable=2 timeout=2 (frame_seq%4==2/3)");
        check(st.failed == 0 && st.dropped == 0, "统计: failed=0 dropped=0");

        std::lock_guard<std::mutex> lk(mtx);
        check(got.size() == 10, "回调收到 10 个异步结果(按 frame_seq 可回收)");

        // 告警语义(容易弄反, 这里钉死):
        // Ok + 确认      -> 告警(复核确认了风险)
        // Ok + 否决      -> **不**告警(复核否掉了误报, 这才是复核的意义)
        // 超时 / 不可用  -> 由 alert_on_failure 决定(默认 false; true = 拿不到结论时兜底)
        // 故 10 个任务 = 3 确认 + 3 否决 + 2 超时 + 2 不可用
        // -> 告警 = 3(确认) + 4(兜底) = 7
        int alerts = 0, alert_on_ok = 0, alert_on_fail = 0;
        for (const auto& o : got) {
            if (!o.alert) continue;
            ++alerts;
            if (o.status == ReviewStatus::Ok) ++alert_on_ok;
            else ++alert_on_fail;
        }
        check(alert_on_ok == 3, "确认才告警: 3 个确认 -> 3 条告警");
        check(alert_on_fail == 4,
              "兜底告警: alert_on_failure=true 时 2 超时 + 2 不可用 -> 4 条兜底告警");
        check(alerts == 7, "合计 7/10 告警 = 3 确认 + 4 兜底; 3 个**否决**一条都不告警");
    }

    // C2: alert_on_failure = false (只有确认才告警)
    {
        ReviewConfig cfg;
        cfg.enabled = true;
        cfg.queue_size = 64;
        cfg.worker_threads = 2;
        cfg.timeout_ms = 1000;
        cfg.alert_on_failure = false;

        auto svc = std::make_shared<FakeReviewer>();
        ReviewScheduler sched;
        std::mutex mtx;
        std::vector<ReviewOutcome> got;
        sched.init(cfg, svc, [&](const ReviewOutcome& o) {
            std::lock_guard<std::mutex> lk(mtx);
            got.push_back(o);
        });

        for (std::uint64_t seq = 0; seq < 4; ++seq) {
            ReviewRequest job;
            job.frame_seq = seq;
            job.roi = cv::Mat::zeros(64, 64, CV_8UC3);
            job.label = "person";
            job.confidence = 0.72f;
            sched.submit(std::move(job));
        }
        sched.stop();

        std::lock_guard<std::mutex> lk(mtx);
        int alerts = 0, alert_on_fail = 0;
        for (const auto& o : got) {
            if (!o.alert) continue;
            ++alerts;
            if (o.status != ReviewStatus::Ok) ++alert_on_fail;
        }
        check(got.size() == 4 && alerts == 1 && alert_on_fail == 0,
              "alert_on_failure=false -> 4 个任务里只有 1 个(确认)告警, 拿不到结论的 2 个不兜底");
    }

    note("真实实现 GrpcLlmReviewer 走 gRPC(proto/review.proto); 服务端不在本仓库。");
    note("没有服务端时全部得到 Unavailable -> 这正是日志里 '复核统计: unavailable=N' 的来源。");
    note("想看到真实的 确认/否决, 可跑 scripts/mock_review_server.py (Python mock)。");
}

// Phase D: 多模态决策级融合 (T23-T26)
void phaseD() {
    section("Phase D-1: 时间对齐 + 目标关联 + 置信度融合 (T25)");

    FusionConfig cfg;
    cfg.enabled = true;
    cfg.level = "decision";
    cfg.time_tolerance_ms = 50;
    cfg.match_iou = 0.30f;
    cfg.sensor_weight = 0.35f;
    cfg.emit_sensor_only = false;
    cfg.buffer_capacity = 256;

    fusion::SensorFusion fus(cfg);

    const std::int64_t anchor = sensor::nowMs();

    // 视觉: person(0.60) + car(0.70)
    std::vector<DetectionResult> vision = {
        mkDet(0, "person", 0.60f, cv::Rect(100, 100, 120, 240)),
        mkDet(2, "car",    0.70f, cv::Rect(400, 100, 200, 150)),
    };

    // 传感器: 红外(带框, 与 person 重叠) + 雷达(无框) + 一个窗口外的旧采样
    std::vector<sensor::SensorSample> samples;
    {
        sensor::SensorTarget t1;
        t1.class_id = 0;
        t1.label = "person";
        t1.confidence = 0.80f;
        t1.box = cv::Rect2f(100, 100, 120, 240);
        t1.distance_m = 6.2f;

        sensor::SensorSample s1;
        s1.kind = sensor::SensorKind::Infrared;
        s1.timestamp_ms = anchor;
        s1.targets.push_back(t1);

        sensor::SensorTarget t2;
        t2.class_id = 0;
        t2.label = "person"; // 无框(雷达) -> 走"标签关联"分支
        t2.confidence = 0.55f;
        t2.distance_m = 11.0f;

        sensor::SensorSample s2;
        s2.kind = sensor::SensorKind::Radar;
        s2.timestamp_ms = anchor + 20;
        s2.targets.push_back(t2);

        sensor::SensorTarget t3 = t2; // 5 秒前的旧采样: 应被时间窗过滤掉
        t3.confidence = 0.99f;
        sensor::SensorSample s3;
        s3.kind = sensor::SensorKind::Radar;
        s3.timestamp_ms = anchor - 5000;
        s3.targets.push_back(t3);

        samples = {s1, s2, s3};
    }

    check(fusion::SensorFusion::align(samples, anchor, cfg.time_tolerance_ms).size() == 2,
          "时间对齐: 3 个采样中 2 个落入 ±50ms 窗口(旧采样被丢弃)");

    const fusion::FusionStats fs = fus.fuse(vision, samples, anchor);
    check(fs.samples_seen == 3 && fs.aligned == 2 && fs.targets == 2,
          "统计: samples_seen=3 aligned=2 targets=2");
    check(fs.matched == 1, "关联: 1 对(红外 person 与视觉 person, IoU≈1.0 >= 0.30)");
    check(fs.unmatched_sensor == 1,
          "未关联: 1(雷达点已被红外占用 -> 贪心抑制'一个点被多框争用')");
    check(fs.emitted_sensor_only == 0, "未关联传感器目标默认不输出(只计数)");

    const DetectionResult& p = vision[0];
    check(p.fused && near(p.vision_confidence, 0.60f) && near(p.sensor_confidence, 0.80f),
          "融合元数据: fused=true, vision_confidence=0.60, sensor_confidence=0.80");
    check(near(p.confidence, 0.67f),
          "置信度融合: (1-0.35)*0.60 + 0.35*0.80 = 0.67");
    check(near(p.distance_m, 6.2f), "测距透传: distance_m=6.2");
    check(!vision[1].fused && near(vision[1].confidence, 0.70f),
          "未关联的视觉目标(car)保持原样, 置信度未被改动");

    // D-2: emit_sensor_only=true -> 未关联且自带框的传感器目标追加为新目标
    {
        FusionConfig c2 = cfg;
        c2.emit_sensor_only = true;
        fusion::SensorFusion fus2(c2);

        std::vector<DetectionResult> v2 = {
            mkDet(0, "person", 0.60f, cv::Rect(10, 10, 50, 50)),
        };
        sensor::SensorTarget t;
        t.class_id = 0;
        t.label = "person";
        t.confidence = 0.70f;
        t.box = cv::Rect2f(900, 600, 100, 200); // 与视觉框完全不重叠
        t.distance_m = 4.0f;
        sensor::SensorSample s;
        s.kind = sensor::SensorKind::Infrared;
        s.timestamp_ms = sensor::nowMs();
        s.targets.push_back(t);

        const fusion::FusionStats f2 = fus2.fuse(v2, {s}, sensor::nowMs());
        check(f2.matched == 0 && f2.unmatched_sensor == 1 && f2.emitted_sensor_only == 1,
              "emit_sensor_only=true: 未关联目标被追加(emitted_sensor_only=1)");
        check(v2.size() == 2 && v2[1].fused && near(v2[1].vision_confidence, -1.0f),
              "追加的纯传感器目标: fused=true, vision_confidence=-1(无视觉置信度)");
    }

    // D-3: MultiSensorPipeline + 真实 poller 线程(stub 传感器)
    section("Phase D-2: 多传感器编排 MultiSensorPipeline (T26)");

    FusionConfig pc = cfg;
    pc.time_tolerance_ms = 100;
    pc.buffer_capacity = 4; // 故意设小, 便于观察"有界缓冲丢最旧"

    auto radar = std::make_shared<sensor::ReplaySensorSource>("radar_front",
                                                             sensor::SensorKind::Radar);
    auto ir = std::make_shared<sensor::ReplaySensorSource>("ir_1",
                                                           sensor::SensorKind::Infrared);
    SensorConfig rc;
    rc.kind = "radar";
    rc.name = "radar_front";
    rc.backend = "stub";
    rc.rate_hz = 20;
    rc.labels = {"person"};
    SensorConfig ic = rc;
    ic.kind = "infrared";
    ic.name = "ir_1";

    check(radar->open(rc) && ir->open(ic), "两路 stub 传感器 open() 成功(无需任何硬件)");

    MultiSensorPipeline mp;
    check(mp.init(pc, {radar, ir}, nullptr),
          "MultiSensorPipeline.init() 成功(poller 线程已启动)");

    int matched_frames = 0;
    for (int i = 0; i < 30; ++i) {
        std::vector<DetectionResult> dets = {
            mkDet(0, "person", 0.60f, cv::Rect(100, 120, 120, 240)),
        };
        mp.fuse(dets); // sink 线程语义: 就地融合, 不阻塞
        if (!dets.empty() && dets[0].fused) ++matched_frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    mp.stop();

    const MultiSensorPipeline::Stats ms = mp.stats();
    check(ms.frames == 30, "融合帧数 frames=30");
    check(ms.samples_pushed > 0,
          "poller 线程推入采样: samples=" + std::to_string(ms.samples_pushed));
    check(ms.fusion.matched > 0,
          "融合命中: matched=" + std::to_string(ms.fusion.matched) +
              " (命中帧数=" + std::to_string(matched_frames) + "/30)");
    check(ms.buffer_size <= pc.buffer_capacity, "有界缓冲未被突破(buffer_size<=capacity)");
    note("samples_dropped=" + std::to_string(ms.samples_dropped) +
         ", poll_errors=" + std::to_string(ms.poll_errors) +
         " —— 缓冲满即丢最旧, 保证 poller 永不阻塞(实时性优先)。");
    note("真实链路: main.cpp 里 fuse() 挂在 sink 最前置, 画框/落库/告警看到的都是融合后置信度。");
}

} // namespace

int main() {
    std::cout << "===== CVInfer-Gate 阶段自检: Phase A~D =====\n";
    std::cout << "(不依赖模型/传感器/复核服务/数据库, 也不需要 config.yaml)\n";

    LogConfig lc;
    lc.level = "info"; // 保留各模块 init 日志, 便于确认线程/资源真的起来了
    Logger::instance().init(lc);

    phaseA();
    phaseB();
    phaseC();
    phaseD();

    std::cout << "\n===== 结果: " << g_pass << " 项通过, " << g_fail << " 项失败 =====\n";
    if (g_fail == 0) {
        std::cout << "Phase A(抽象层) / B(级联灰区) / C(异步复核) / D(多模态融合) 全部行为已演示。\n";
    }
    return g_fail == 0 ? 0 : 1;
}
