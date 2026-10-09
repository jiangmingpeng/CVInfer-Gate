// ConfigParser 单元测试
// 为什么值得测: 配置是**唯一**能"静默把系统调成另一个东西"的输入 ——
// 线程数、灰区阈值、融合开关、复核地址全在这里。本项目已经写了不少校验
// (含交叉校验, 如 source_type 与 source_path 前缀是否自相矛盾), 但此前
// **零自动化覆盖**: 误改一条校验规则, 没有任何东西会报警。
//
// 测试策略: 走公开入口(loadAppConfig / loadModelConfig), 用临时 YAML 文件
// 驱动 —— 这样连"解析 + 环境变量展开 + 校验"整条链一起覆盖, 而不是只测
// 某个 private 函数。
//
// 覆盖: 最小合法配置 + 默认值 / 缺文件 / 类型与前缀交叉校验 / 日志与队列
// 枚举校验 / ${VAR} 与 ${VAR:-default} 展开 / 级联灰区颠倒 / 复核缺
// endpoint / 传感器各种非法组合 / 多模型新格式 / 单模型旧格式兼容 /
// 角色与性能模式校验 / 复核场景 ROI 策略(scale / min_side / dwell) /
// 占座判定(座位两种写法 / 物品标签 / 时序与投票参数 / 各类非法组合)。
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "utils/ConfigParser.h"

namespace {

// 把 YAML 文本落到临时文件, 返回路径(每次用不同文件名, 避免相互覆盖)
std::string writeTempYaml(const std::string& name, const std::string& content) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream ofs(path, std::ios::trunc);
    ofs << content;
    ofs.close();
    return path.string();
}

// 一份"最小可用"的系统配置: 只有 video 是必填的
const char* kMinimalAppYaml = R"(
video:
  source_type: file
  source_path: /tmp/unit_test.mp4
)";

} // namespace

// ---------------------------------------------------------------- 系统配置

TEST(ConfigParserApp, LoadsMinimalConfigAndKeepsDocumentedDefaults) {
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_min.yaml", kMinimalAppYaml)));

    const AppConfig& c = p.getAppConfig();
    EXPECT_EQ(c.video.source_type, "file");
    EXPECT_EQ(c.video.source_path, "/tmp/unit_test.mp4");
    // 默认值本身也是契约: 少配一项不该改变行为
    EXPECT_EQ(c.log.level, "info");
    EXPECT_EQ(c.pipeline.worker_threads, 2);
    EXPECT_EQ(c.database.pool_size, 4);
    EXPECT_EQ(c.grpc.port, 50051);
    EXPECT_EQ(c.video.queue.max_size, 24u);
    EXPECT_EQ(c.video.queue.policy, "drop_oldest");
    // 四个增强层默认全关 => 与纯视觉链路行为一致
    EXPECT_FALSE(c.cascade.enabled);
    EXPECT_FALSE(c.review.enabled);
    EXPECT_FALSE(c.fusion.enabled);
    EXPECT_TRUE(c.sensors.empty());
    // 告警去重: 默认**开启** —— 这是刻意的行为修正(关掉就退回"每帧都告警")
    EXPECT_TRUE(c.alert.dedup.enabled);
    EXPECT_FLOAT_EQ(c.alert.dedup.iou, 0.30f);
    EXPECT_EQ(c.alert.dedup.cooldown_ms, 5000);
    EXPECT_EQ(c.alert.dedup.max_entries, 256);
    // 目标跟踪: 同样默认**开启**(关掉就退回几何去重)
    EXPECT_TRUE(c.tracking.enabled);
    EXPECT_FLOAT_EQ(c.tracking.iou, 0.30f);
    EXPECT_FLOAT_EQ(c.tracking.dist_factor, 1.0f);
    EXPECT_EQ(c.tracking.max_age_ms, 1000);
    EXPECT_EQ(c.tracking.min_hits, 1);
    EXPECT_EQ(c.tracking.max_tracks, 256);
}

TEST(ConfigParserApp, RejectsMissingFile) {
    ConfigParser p;
    EXPECT_FALSE(p.loadAppConfig("/definitely/not/here.yaml"));
}

TEST(ConfigParserApp, RejectsSourceTypePathMismatch) {
    // 高频手误: 换了视频源却忘了改 source_type —— 校验必须拦住,
    // 否则会静默走错源(旧版本的真实坑)
    ConfigParser p;
    EXPECT_FALSE(p.loadAppConfig(writeTempYaml("cv_ut_mis1.yaml", R"(
video:
  source_type: file
  source_path: rtsp://127.0.0.1:8554/live
)")));

    ConfigParser q;
    EXPECT_FALSE(q.loadAppConfig(writeTempYaml("cv_ut_mis2.yaml", R"(
video:
  source_type: rtsp
  source_path: test.mp4
)")));
}

TEST(ConfigParserApp, RejectsInvalidEnums) {
    ConfigParser log_level;
    EXPECT_FALSE(log_level.loadAppConfig(writeTempYaml("cv_ut_log.yaml", R"(
app:
  log_level: verbose
video:
  source_type: file
  source_path: /tmp/a.mp4
)")));

    ConfigParser queue_policy;
    EXPECT_FALSE(queue_policy.loadAppConfig(writeTempYaml("cv_ut_qp.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
  queue:
    policy: drop_newest
)")));
}

TEST(ConfigParserApp, RejectsNumericOutOfRange) {
    ConfigParser frame_interval;
    EXPECT_FALSE(frame_interval.loadAppConfig(writeTempYaml("cv_ut_fi.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
  frame_interval: 0
)")));

    ConfigParser workers;
    EXPECT_FALSE(workers.loadAppConfig(writeTempYaml("cv_ut_wt.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
pipeline:
  worker_threads: 0
)")));

    ConfigParser port;
    EXPECT_FALSE(port.loadAppConfig(writeTempYaml("cv_ut_port.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
grpc:
  port: 70000
)")));
}

TEST(ConfigParserApp, ExpandsEnvVarThenFallsBackToDefault) {
    ::setenv("CVINFER_UT_PATH", "/tmp/from_env.mp4", 1);
    ConfigParser with_env;
    ASSERT_TRUE(with_env.loadAppConfig(writeTempYaml("cv_ut_env1.yaml", R"(
video:
  source_type: file
  source_path: ${CVINFER_UT_PATH:-/tmp/from_default.mp4}
)")));
    EXPECT_EQ(with_env.getAppConfig().video.source_path, "/tmp/from_env.mp4");

    ::unsetenv("CVINFER_UT_PATH");
    ConfigParser without_env;
    ASSERT_TRUE(without_env.loadAppConfig(writeTempYaml("cv_ut_env2.yaml", R"(
video:
  source_type: file
  source_path: ${CVINFER_UT_PATH:-/tmp/from_default.mp4}
)")));
    EXPECT_EQ(without_env.getAppConfig().video.source_path, "/tmp/from_default.mp4");
}

TEST(ConfigParserApp, KeepsLiteralWhenEnvUndefinedAndNoDefault) {
    ::unsetenv("CVINFER_UT_UNSET");
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_env3.yaml", R"(
video:
  source_type: file
  source_path: ${CVINFER_UT_UNSET}
)")));
    // 保留原样 -> 便于排查"配置没生效"类问题(而不是变成空字符串后静默通过)
    EXPECT_EQ(p.getAppConfig().video.source_path, "${CVINFER_UT_UNSET}");
}

TEST(ConfigParserApp, RejectsCascadeWithInvertedGrayZone) {
    ConfigParser p;
    EXPECT_FALSE(p.loadAppConfig(writeTempYaml("cv_ut_cas.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
cascade:
  enabled: true
  trigger:
    labels: ["person"]
    min_conf: 0.9
    max_conf: 0.1
)")));
}

TEST(ConfigParserApp, RejectsReviewEnabledWithoutEndpoint) {
    ConfigParser p;
    EXPECT_FALSE(p.loadAppConfig(writeTempYaml("cv_ut_rev.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
review:
  enabled: true
  endpoint: ""
)")));
}

TEST(ConfigParserApp, RejectsInvalidSensorConfigurations) {
    // 传感器重名
    ConfigParser dup;
    EXPECT_FALSE(dup.loadAppConfig(writeTempYaml("cv_ut_sen1.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
sensors:
  - kind: radar
    name: front
    backend: stub
  - kind: radar
    name: front
    backend: stub
)")));

    // 视频不能在 sensors: 里重复声明(否则两处时间基不一致)
    ConfigParser video_kind;
    EXPECT_FALSE(video_kind.loadAppConfig(writeTempYaml("cv_ut_sen2.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
sensors:
  - kind: video
    name: cam0
    backend: stub
)")));

    // backend=file 必须给 path
    ConfigParser file_backend;
    EXPECT_FALSE(file_backend.loadAppConfig(writeTempYaml("cv_ut_sen3.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
sensors:
  - kind: infrared
    name: ir0
    backend: file
)")));

    // 开了融合却没配任何传感器
    ConfigParser no_sensors;
    EXPECT_FALSE(no_sensors.loadAppConfig(writeTempYaml("cv_ut_sen4.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
fusion:
  enabled: true
)")));
}

TEST(ConfigParserApp, ParsesAlertDedupOverrides) {
    // YAML 里的 alert.dedup 必须真的被读进去(不能只是默认值恰好一致)
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_gate_ok.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
alert:
  dedup:
    enabled: false
    iou: 0.5
    cooldown_ms: 12000
    max_entries: 8
)")));
    const AppConfig& c = p.getAppConfig();
    EXPECT_FALSE(c.alert.dedup.enabled);
    EXPECT_FLOAT_EQ(c.alert.dedup.iou, 0.5f);
    EXPECT_EQ(c.alert.dedup.cooldown_ms, 12000);
    EXPECT_EQ(c.alert.dedup.max_entries, 8);
}

TEST(ConfigParserApp, RejectsInvalidAlertDedup) {
    // iou 越界 / cooldown 为负 / max_entries < 1 都必须被拒
    ConfigParser bad_iou;
    EXPECT_FALSE(bad_iou.loadAppConfig(writeTempYaml("cv_ut_gate1.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
alert:
  dedup:
    iou: 1.5
)")));

    ConfigParser bad_cd;
    EXPECT_FALSE(bad_cd.loadAppConfig(writeTempYaml("cv_ut_gate2.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
alert:
  dedup:
    cooldown_ms: -1
)")));

    ConfigParser bad_max;
    EXPECT_FALSE(bad_max.loadAppConfig(writeTempYaml("cv_ut_gate3.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
alert:
  dedup:
    max_entries: 0
)")));
}

TEST(ConfigParserApp, ParsesTrackingOverridesAndRejectsBadValues) {
    // YAML 里的 tracking 必须真的被读进去
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_trk_ok.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
tracking:
  enabled: false
  iou: 0.5
  dist_factor: 0.0
  max_age_ms: 2000
  min_hits: 3
  max_tracks: 16
)")));
    const AppConfig& c = p.getAppConfig();
    EXPECT_FALSE(c.tracking.enabled);
    EXPECT_FLOAT_EQ(c.tracking.iou, 0.5f);
    EXPECT_FLOAT_EQ(c.tracking.dist_factor, 0.0f);
    EXPECT_EQ(c.tracking.max_age_ms, 2000);
    EXPECT_EQ(c.tracking.min_hits, 3);
    EXPECT_EQ(c.tracking.max_tracks, 16);

    ConfigParser bad_iou;
    EXPECT_FALSE(bad_iou.loadAppConfig(writeTempYaml("cv_ut_trk1.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
tracking:
  iou: 2.0
)")));

    ConfigParser bad_hits;
    EXPECT_FALSE(bad_hits.loadAppConfig(writeTempYaml("cv_ut_trk2.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
tracking:
  min_hits: 0
)")));

    ConfigParser bad_age;
    EXPECT_FALSE(bad_age.loadAppConfig(writeTempYaml("cv_ut_trk3.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
tracking:
  max_age_ms: -5
)")));
}

TEST(ConfigParserApp, AcceptsFullFeatureConfig) {
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_full.yaml", R"(
video:
  source_type: rtsp
  source_path: rtsp://127.0.0.1:8554/live
  target_fps: 10
  frame_interval: 2
  queue:
    max_size: 8
    policy: block
pipeline:
  worker_threads: 2
cascade:
  enabled: true
  primary: yolov8_detector
  secondary: helmet_classifier
  trigger:
    labels: ["person"]
    min_conf: 0.40
    max_conf: 0.90
  accept_label: safety_helmet
  accept_conf: 0.50
review:
  enabled: true
  endpoint: "127.0.0.1:50052"
  trigger:
    labels: ["person"]
    min_conf: 0.50
    max_conf: 1.00
sensors:
  - kind: radar
    name: radar_front
    backend: stub
    rate_hz: 10
    labels: ["person"]
fusion:
  enabled: true
  level: decision
  time_tolerance_ms: 50
  match_iou: 0.30
  sensor_weight: 0.35
)")));

    const AppConfig& c = p.getAppConfig();
    EXPECT_EQ(c.video.queue.policy, "block");
    EXPECT_EQ(c.pipeline.worker_threads, 2);
    EXPECT_TRUE(c.cascade.enabled);
    EXPECT_EQ(c.cascade.secondary, "helmet_classifier");
    ASSERT_EQ(c.cascade.trigger_labels.size(), 1u);
    EXPECT_EQ(c.cascade.trigger_labels[0], "person");
    EXPECT_FLOAT_EQ(c.cascade.min_conf, 0.40f);
    EXPECT_TRUE(c.review.enabled);
    ASSERT_EQ(c.sensors.size(), 1u);
    EXPECT_EQ(c.sensors[0].name, "radar_front");
    EXPECT_EQ(c.sensors[0].rate_hz, 10);
    EXPECT_TRUE(c.fusion.enabled);
    EXPECT_FLOAT_EQ(c.fusion.sensor_weight, 0.35f);
}

// ---------------------------------------------------------------- 复核 ROI 策略

TEST(ConfigParserApp, ParsesReviewRoiStrategyOverrides) {
    // YAML 里的新键必须真的被读进去(不能只是默认值恰好一致)
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_rev_roi_ok.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
review:
  enabled: true
  endpoint: "127.0.0.1:50052"
  roi_padding: 0.25
  roi_context_scale: 3.0
  roi_min_side: 320
  min_dwell_ms: 2000
)")));

    const ReviewConfig& r = p.getAppConfig().review;
    EXPECT_FLOAT_EQ(r.roi_padding, 0.25f);
    EXPECT_FLOAT_EQ(r.roi_context_scale, 3.0f);
    EXPECT_EQ(r.roi_min_side, 320);
    EXPECT_EQ(r.min_dwell_ms, 2000);
}

TEST(ConfigParserApp, ReviewRoiStrategyDefaultsKeepLegacyBehaviour) {
    // 不写新键 => 必须与改造前行为一致: 不放大 / 不补边长 / 不做时序门。
    // 这条是"加了旋钮但默认关掉"的保险: 一旦默认值被改, 占座之外的老业务
    // 会静默改变送审图像内容。
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_rev_roi_def.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
review:
  enabled: true
  endpoint: "127.0.0.1:50052"
)")));

    const ReviewConfig& r = p.getAppConfig().review;
    EXPECT_FLOAT_EQ(r.roi_context_scale, 1.0f);
    EXPECT_EQ(r.roi_min_side, 0);
    EXPECT_EQ(r.min_dwell_ms, 0);
}

TEST(ConfigParserApp, RejectsInvalidReviewRoiStrategy) {
    // scale < 1 会"缩小"送审图(语义上无意义) => 拒; 负的 min_side/dwell 同理
    ConfigParser bad_scale;
    EXPECT_FALSE(bad_scale.loadAppConfig(writeTempYaml("cv_ut_rev_roi1.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
review:
  enabled: true
  endpoint: "127.0.0.1:50052"
  roi_context_scale: 0.5
)")));

    ConfigParser bad_side;
    EXPECT_FALSE(bad_side.loadAppConfig(writeTempYaml("cv_ut_rev_roi2.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
review:
  enabled: true
  endpoint: "127.0.0.1:50052"
  roi_min_side: -10
)")));

    ConfigParser bad_dwell;
    EXPECT_FALSE(bad_dwell.loadAppConfig(writeTempYaml("cv_ut_rev_roi3.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
review:
  enabled: true
  endpoint: "127.0.0.1:50052"
  min_dwell_ms: -1
)")));
}

// ---------------------------------------------------------------- 占座判定

TEST(ConfigParserApp, OccupancyDefaultsOffWithNoSeats) {
    // 关键默认: 不写 occupancy: 段 => 完全关闭。这是"新增能力不该改老行为"的保险。
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_occ_def.yaml", kMinimalAppYaml)));
    const OccupancyConfig& o = p.getAppConfig().occupancy;
    EXPECT_FALSE(o.enabled);
    EXPECT_TRUE(o.seats.empty());
    EXPECT_TRUE(o.item_labels.empty()); // 空 => 由组件用内置默认标签
    EXPECT_EQ(o.person_label, "person");
    EXPECT_FLOAT_EQ(o.item_seat_overlap, 0.5f);
    EXPECT_FLOAT_EQ(o.item_person_overlap, 0.5f);
    // C2 包含度默认从 0.15 收紧到 0.30: 15% 太松会让邻座/路人框误命中 =>
    // 占座计时被反复暂停, 真占座漏报。
    EXPECT_FLOAT_EQ(o.person_seat_iou, 0.30f);
    EXPECT_EQ(o.min_person_height_px, 0);
    EXPECT_EQ(o.t_occupied_ms, 300000); // 5 分钟(业务口径, 上线前需业务方拍板)
    EXPECT_EQ(o.t_grace_ms, 90000);
    EXPECT_EQ(o.vote_n, 10);
    EXPECT_EQ(o.vote_m, 7);
}

TEST(ConfigParserApp, ParsesOccupancyOverridesWithBothSeatForms) {
    ConfigParser p;
    ASSERT_TRUE(p.loadAppConfig(writeTempYaml("cv_ut_occ_ok.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
  item_labels: ["book", "bag"]
  person_label: person
  item_seat_overlap: 0.6
  item_person_overlap: 0.4
  person_seat_iou: 0.2
  min_person_height_px: 80
  t_occupied_ms: 60000
  t_grace_ms: 10000
  vote_n: 5
  vote_m: 3
  seats:
    - name: A-12
      rect: [100, 100, 120, 120]
    - name: A-13
      polygon: [[300, 300], [420, 300], [420, 420], [300, 420]]
)")));

    const OccupancyConfig& o = p.getAppConfig().occupancy;
    EXPECT_TRUE(o.enabled);
    ASSERT_EQ(o.item_labels.size(), 2u);
    EXPECT_EQ(o.item_labels[0], "book");
    EXPECT_FLOAT_EQ(o.item_seat_overlap, 0.6f);
    EXPECT_FLOAT_EQ(o.item_person_overlap, 0.4f);
    EXPECT_FLOAT_EQ(o.person_seat_iou, 0.2f);
    EXPECT_EQ(o.min_person_height_px, 80);
    EXPECT_EQ(o.t_occupied_ms, 60000);
    EXPECT_EQ(o.t_grace_ms, 10000);
    EXPECT_EQ(o.vote_n, 5);
    EXPECT_EQ(o.vote_m, 3);

    ASSERT_EQ(o.seats.size(), 2u);
    EXPECT_EQ(o.seats[0].name, "A-12");
    // rect 写法在配置层就被展开为 4 个顶点 => 运行期只需处理多边形一种形态
    ASSERT_EQ(o.seats[0].polygon.size(), 4u);
    EXPECT_EQ(o.seats[0].polygon[0].x, 100);
    EXPECT_EQ(o.seats[0].polygon[0].y, 100);
    EXPECT_EQ(o.seats[0].polygon[2].x, 220);
    EXPECT_EQ(o.seats[0].polygon[2].y, 220);

    EXPECT_EQ(o.seats[1].name, "A-13");
    ASSERT_EQ(o.seats[1].polygon.size(), 4u);
    EXPECT_EQ(o.seats[1].polygon[0].x, 300);
    EXPECT_EQ(o.seats[1].polygon[0].y, 300);
}

TEST(ConfigParserApp, RejectsOccupancyEnabledWithoutSeats) {
    // 座位靠配置画出 ---- 开了占座却没画座位, 系统会一个事件都产生不了(静默失效),
    // 这是最难排查的一类故障 => 必须在配置层直接拦住。
    ConfigParser p;
    EXPECT_FALSE(p.loadAppConfig(writeTempYaml("cv_ut_occ1.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
)")));
}

TEST(ConfigParserApp, RejectsOccupancyBadSeats) {
    // 顶点不足 3 个
    ConfigParser few_points;
    EXPECT_FALSE(few_points.loadAppConfig(writeTempYaml("cv_ut_occ2.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
  seats:
    - name: bad
      polygon: [[10, 10], [20, 20]]
)")));

    // 座位重名: 日志/告警/去重会互相串味
    ConfigParser dup_names;
    EXPECT_FALSE(dup_names.loadAppConfig(writeTempYaml("cv_ut_occ3.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
  seats:
    - name: same
      rect: [0, 0, 10, 10]
    - name: same
      rect: [20, 20, 10, 10]
)")));

    // 负坐标
    ConfigParser negative;
    EXPECT_FALSE(negative.loadAppConfig(writeTempYaml("cv_ut_occ4.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
  seats:
    - name: neg
      polygon: [[-5, 10], [20, 10], [20, 20]]
)")));
}

TEST(ConfigParserApp, RejectsOccupancyBadNumbersAndLabelCollision) {
    // vote_m > vote_n
    ConfigParser bad_vote;
    EXPECT_FALSE(bad_vote.loadAppConfig(writeTempYaml("cv_ut_occ5.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
  vote_n: 3
  vote_m: 5
  seats:
    - name: s
      rect: [0, 0, 10, 10]
)")));

    // 重叠阈值越界
    ConfigParser bad_overlap;
    EXPECT_FALSE(bad_overlap.loadAppConfig(writeTempYaml("cv_ut_occ6.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  item_seat_overlap: 1.5
)")));

    // 持续时间不能为负
    ConfigParser bad_dwell;
    EXPECT_FALSE(bad_dwell.loadAppConfig(writeTempYaml("cv_ut_occ7.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  t_occupied_ms: -1
)")));

    // item_labels 里混进 person => 同一目标既是"物品"又是"人" => 状态机自相矛盾
    ConfigParser label_collision;
    EXPECT_FALSE(label_collision.loadAppConfig(writeTempYaml("cv_ut_occ8.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  person_label: person
  item_labels: ["book", "person"]
)")));
}

TEST(ConfigParserApp, RejectsOverlappingOccupancySeats) {
    // 座位重叠 => 同一个物品/人会被两个座位**同时**命中(乱覆盖/漏报)。运行期虽有
    // "排他归属"兜底, 但重叠本身几乎总是 zone 画错的信号 => 配置层就要拦住。
    ConfigParser overlap;
    EXPECT_FALSE(overlap.loadAppConfig(writeTempYaml("cv_ut_occ_ovl.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
  seats:
    - name: L
      rect: [100, 100, 160, 160]
    - name: R
      rect: [160, 100, 160, 160]
)")));

    // 紧邻但不重叠(仅共边)是合法的 —— 联排桌常见画法, 不能误报
    ConfigParser touching;
    EXPECT_TRUE(touching.loadAppConfig(writeTempYaml("cv_ut_occ_touch.yaml", R"(
video:
  source_type: file
  source_path: /tmp/a.mp4
occupancy:
  enabled: true
  seats:
    - name: L
      rect: [100, 100, 160, 160]
    - name: R
      rect: [260, 100, 160, 160]
)")));
}

// ---------------------------------------------------------------- 模型配置

TEST(ConfigParserModel, LoadsMultiModelListAndTreatsFirstAsPrimary) {
    ConfigParser p;
    ASSERT_TRUE(p.loadModelConfig(writeTempYaml("cv_ut_models.yaml", R"(
models:
  - name: yolov8_detector
    role: detector
    xml_path: models/yolov8n.xml
    bin_path: models/yolov8n.bin
    labels_path: models/labels.txt
    conf: 0.30
    nms: 0.50
    pool_size: 2
    device: CPU
    performance_mode: LATENCY
    num_threads: 4
  - name: helmet_classifier
    role: classifier
    xml_path: models/helmet_cls.xml
    bin_path: models/helmet_cls.bin
    labels_path: models/helmet_labels.txt
    input_width: 416
    input_height: 416
)")));

    const auto& all = p.getModelConfigs();
    ASSERT_EQ(all.size(), 2u);

    EXPECT_EQ(all[0].name, "yolov8_detector");
    EXPECT_EQ(all[0].role, "detector");
    EXPECT_FLOAT_EQ(all[0].conf_threshold, 0.30f);
    EXPECT_FLOAT_EQ(all[0].nms_threshold, 0.50f);
    EXPECT_EQ(all[0].pool_size, 2);
    EXPECT_EQ(all[0].num_threads, 4);
    EXPECT_EQ(all[0].perf_mode, "latency"); // 大写输入被规整为小写

    EXPECT_EQ(all[1].role, "classifier");
    EXPECT_EQ(all[1].input_width, 416);
    EXPECT_EQ(all[1].input_height, 416);
    // device 未写 -> 保留 "AUTO"(注意: 生产配置**必须**显式写 CPU, 否则 WSL+NPU 会崩)
    EXPECT_EQ(all[1].device, "AUTO");

    // 旧接口语义: getModelConfig() = 首个 = 主模型
    EXPECT_EQ(p.getModelConfig().name, "yolov8_detector");
}

TEST(ConfigParserModel, StillAcceptsLegacySingleModelFormat) {
    ConfigParser p;
    ASSERT_TRUE(p.loadModelConfig(writeTempYaml("cv_ut_legacy.yaml", R"(
model:
  xml_path: models/yolov8n.xml
  bin_path: models/yolov8n.bin
  labels_path: models/labels.txt
thresholds:
  conf: 0.25
  nms: 0.45
)")));

    const auto& all = p.getModelConfigs();
    ASSERT_EQ(all.size(), 1u); // 单模型被规整为长度 1 的列表
    EXPECT_EQ(all[0].name, "yolov8_detector");
    EXPECT_EQ(all[0].role, "detector");
    EXPECT_EQ(all[0].model_xml_path, "models/yolov8n.xml");
    EXPECT_FLOAT_EQ(all[0].conf_threshold, 0.25f);
    EXPECT_FLOAT_EQ(all[0].nms_threshold, 0.45f);
    EXPECT_EQ(all[0].pool_size, 0);
    EXPECT_EQ(all[0].input_width, 640);
}

TEST(ConfigParserModel, RejectsMissingXmlPath) {
    ConfigParser p;
    EXPECT_FALSE(p.loadModelConfig(writeTempYaml("cv_ut_m1.yaml", R"(
models:
  - name: broken
    role: detector
)")));
}

TEST(ConfigParserModel, RejectsDuplicateModelNames) {
    ConfigParser p;
    EXPECT_FALSE(p.loadModelConfig(writeTempYaml("cv_ut_m2.yaml", R"(
models:
  - name: same_name
    role: detector
    xml_path: models/a.xml
  - name: same_name
    role: classifier
    xml_path: models/b.xml
)")));
}

TEST(ConfigParserModel, RejectsInvalidRoleIncludingDeprecatedReviewer) {
    ConfigParser bogus_role;
    EXPECT_FALSE(bogus_role.loadModelConfig(writeTempYaml("cv_ut_m3.yaml", R"(
models:
  - name: seg
    role: segmenter
    xml_path: models/a.xml
)")));

    // role=reviewer 已废弃: 大模型复核走 config.yaml 的 review: 段, 不放 models:
    ConfigParser reviewer;
    EXPECT_FALSE(reviewer.loadModelConfig(writeTempYaml("cv_ut_m4.yaml", R"(
models:
  - name: vlm
    role: reviewer
    xml_path: models/a.xml
)")));
}

TEST(ConfigParserModel, RejectsInvalidPerfModeButAcceptsEmptyOne) {
    ConfigParser bogus;
    EXPECT_FALSE(bogus.loadModelConfig(writeTempYaml("cv_ut_m5.yaml", R"(
models:
  - name: det
    role: detector
    xml_path: models/a.xml
    performance_mode: turbo
)")));

    // 不写 performance_mode = 不向 OpenVINO 设任何属性(改造前行为)
    ConfigParser empty_mode;
    ASSERT_TRUE(empty_mode.loadModelConfig(writeTempYaml("cv_ut_m6.yaml", R"(
models:
  - name: det
    role: detector
    xml_path: models/a.xml
)")));
    EXPECT_TRUE(empty_mode.getModelConfig().perf_mode.empty());
    EXPECT_EQ(empty_mode.getModelConfig().device, "AUTO");
}

TEST(ConfigParserModel, RejectsMissingFile) {
    ConfigParser p;
    EXPECT_FALSE(p.loadModelConfig("/definitely/not/here.yaml"));
}
