// 占座判定器单测 (tests/unit/test_seat_occupancy.cpp)
//
// 为什么这个模块值得单测(而整条流水线不值得 —— 见 CMakeLists 的说明):
// 占座规则是**纯逻辑**: 输入(检测框 + 时间戳) -> 输出(状态/事件), 不碰模型、
// 不碰 RTSP、不碰数据库。于是"人离开 5 分钟算不算占座""单帧误检会不会把计时
// 打回原点"这类**业务口径**问题, 可以在这里用确定性用例锁死, 而不是靠
// 守着监控画面试半天。
//
// 座位区约定(各用例共用): 底边中点是"物体落在哪里"的判据, 故
//   zone  = (100,100)-(220,220)
//   item  = (150,180,40,30) => 底边中点 (170,210) 在区内  => C1 成立
//   person= (150,120,40,100) => 底边中点 (170,220) 在区内  => C2 成立
#include "occupancy/SeatOccupancyAnalyzer.h"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace {

using occupancy::OccupancyEvent;
using occupancy::SeatOccupancyAnalyzer;
using occupancy::SeatState;
using occupancy::SeatZone;

SeatZone zoneRect(const std::string& name, int x, int y, int w, int h) {
    SeatZone z;
    z.name = name;
    z.polygon = {cv::Point(x, y), cv::Point(x + w, y), cv::Point(x + w, y + h),
                 cv::Point(x, y + h)};
    return z;
}

DetectionResult makeDet(const std::string& label, const cv::Rect& box, float conf = 0.9f) {
    DetectionResult d;
    d.class_id = 0;
    d.confidence = conf;
    d.box = box;
    d.label = label;
    return d;
}

// 基础配置: 关闭投票(vote_n=1) => 逐帧判定, 便于隔离验证各条标准
occupancy::Config baseCfg() {
    occupancy::Config c;
    c.enabled = true;
    c.item_labels = {"book", "bag", "laptop"};
    c.item_seat_overlap = 0.5f;
    c.item_person_overlap = 0.3f;
    c.person_seat_iou = 0.15f;
    c.min_person_height_px = 0;
    c.t_occupied_ms = 1000;
    c.t_grace_ms = 300;
    c.vote_n = 1;
    c.vote_m = 1;
    return c;
}

// 一个座位 + 单调时钟的用例夹具
class SeatTest : public ::testing::Test {
protected:
    void SetUp() override {
        cfg_ = baseCfg();
        zones_ = {zoneRect("A-12", 100, 100, 120, 120)};
    }
    void build() { an_ = std::make_unique<SeatOccupancyAnalyzer>(cfg_, zones_); }

    // 前进 dt 毫秒, 喂入本帧检测, 返回产生的事件
    std::vector<OccupancyEvent> step(std::int64_t dt, std::vector<DetectionResult> dets) {
        t_ += dt;
        return an_->update(dets, t_);
    }

    occupancy::Config cfg_;
    std::vector<SeatZone> zones_;
    std::unique_ptr<SeatOccupancyAnalyzer> an_;
    std::int64_t t_ = 0;
};

// 两个物品框都"在座位上"(底边中点落在区内), 但只有一个与人框重叠:
//   kItemOnSeat 与人框重叠 => 会被 C1b 判为"手持", 用于验证 C1b;
//   kItemClear  不与人框重叠 => 用于单独验证 C2/C4(否则两条件会互相干扰)。
const cv::Rect kItemOnSeat(150, 180, 40, 30); // 底边中点 (170,210) 在区内
const cv::Rect kItemClear(100, 180, 40, 30);  // 底边中点 (120,210) 在区内, 与人框无交集
const cv::Rect kPersonInZone(150, 120, 40, 100); // 底边中点 (170,220) 在区内

// ---------------------------------------------------------------------------
// 几何判据
// ---------------------------------------------------------------------------

TEST(SeatGeometry, PointInPolygon) {
    const std::vector<cv::Point> poly = {cv::Point(100, 100), cv::Point(220, 100),
                                         cv::Point(220, 220), cv::Point(100, 220)};
    EXPECT_TRUE(SeatOccupancyAnalyzer::pointInPolygon(cv::Point2f(160.f, 160.f), poly));
    EXPECT_FALSE(SeatOccupancyAnalyzer::pointInPolygon(cv::Point2f(50.f, 160.f), poly));
    EXPECT_FALSE(SeatOccupancyAnalyzer::pointInPolygon(cv::Point2f(160.f, 400.f), poly));
    // 边界算"在区内"(与 zone 语义一致: 压线也算那个座位)
    EXPECT_TRUE(SeatOccupancyAnalyzer::pointInPolygon(cv::Point2f(100.f, 160.f), poly));
    // 退化多边形(顶点不足 3)一律为 false, 不误报
    EXPECT_FALSE(SeatOccupancyAnalyzer::pointInPolygon(cv::Point2f(160.f, 160.f),
                                                       {cv::Point(1, 1), cv::Point(2, 2)}));
}

TEST(SeatGeometry, Containment) {
    const std::vector<cv::Point> poly = {cv::Point(100, 100), cv::Point(220, 100),
                                         cv::Point(220, 220), cv::Point(100, 220)};
    // 完全在区内 => 1
    EXPECT_NEAR(SeatOccupancyAnalyzer::containment(cv::Rect(120, 120, 40, 40), poly), 1.0f, 1e-4);
    // 完全在区外 => 0
    EXPECT_FLOAT_EQ(SeatOccupancyAnalyzer::containment(cv::Rect(300, 300, 40, 40), poly), 0.0f);
    // 一半压线(x 180..260, 区右界 220) => 0.5
    EXPECT_NEAR(SeatOccupancyAnalyzer::containment(cv::Rect(180, 140, 80, 40), poly), 0.5f, 1e-3);
    // 退化多边形 => 0
    EXPECT_FLOAT_EQ(SeatOccupancyAnalyzer::containment(cv::Rect(120, 120, 40, 40), {}), 0.0f);
    // 空框 => 0(不除零)
    EXPECT_FLOAT_EQ(SeatOccupancyAnalyzer::containment(cv::Rect(0, 0, 0, 0), poly), 0.0f);
}

TEST(SeatGeometry, Iou) {
    EXPECT_NEAR(SeatOccupancyAnalyzer::iou(cv::Rect(0, 0, 10, 10), cv::Rect(0, 0, 10, 10)), 1.0f,
                1e-4);
    EXPECT_FLOAT_EQ(SeatOccupancyAnalyzer::iou(cv::Rect(0, 0, 10, 10), cv::Rect(50, 50, 10, 10)),
                    0.0f);
    // 空框 => 0
    EXPECT_FLOAT_EQ(SeatOccupancyAnalyzer::iou(cv::Rect(0, 0, 0, 10), cv::Rect(0, 0, 10, 10)),
                    0.0f);
}

TEST(SeatGeometry, ItemLabelMatching) {
    const std::vector<std::string> items = {"book", "bag"};
    EXPECT_TRUE(SeatOccupancyAnalyzer::isItemLabel("book", items));
    EXPECT_FALSE(SeatOccupancyAnalyzer::isItemLabel("person", items));
    EXPECT_FALSE(SeatOccupancyAnalyzer::isItemLabel("book2", items)); // 不做前缀匹配
}

// ---------------------------------------------------------------------------
// C1/C2: 单帧几何 -> 状态
// ---------------------------------------------------------------------------

TEST_F(SeatTest, NoDetectionsStaysIdle) {
    build();
    auto ev = step(0, {});
    EXPECT_TRUE(ev.empty());
    ASSERT_EQ(an_->snapshot().size(), 1u);
    EXPECT_EQ(an_->snapshot()[0].state, SeatState::Idle);
    EXPECT_EQ(an_->snapshot()[0].item_count, 0);
}

TEST_F(SeatTest, ItemOnSeatEntersPendingButNotOccupied) {
    build();
    step(0, {makeDet("book", kItemOnSeat)});
    ASSERT_EQ(an_->snapshot().size(), 1u);
    EXPECT_EQ(an_->snapshot()[0].state, SeatState::Pending); // 还没够久
    EXPECT_EQ(an_->snapshot()[0].item_count, 1);
    EXPECT_EQ(an_->snapshot()[0].item_labels.size(), 1u);
}

TEST_F(SeatTest, ItemOutsideZoneIsIgnored) {
    build();
    // 书在画面里, 但不在这个座位区内 => 与占座无关
    step(0, {makeDet("book", cv::Rect(600, 600, 40, 30))});
    EXPECT_EQ(an_->snapshot()[0].item_count, 0);
    EXPECT_EQ(an_->snapshot()[0].state, SeatState::Idle);
}

TEST_F(SeatTest, NonItemLabelIsIgnored) {
    build();
    // 一个"椅子"落在座位区内 —— 它不是"物品"(椅子是环境, 不是被放下的东西)
    step(0, {makeDet("chair", kItemOnSeat)});
    EXPECT_EQ(an_->snapshot()[0].item_count, 0);
}

TEST_F(SeatTest, PersonInSeatPreventsOccupancy) {
    build();
    // 物品在 + 人也在 => 这是"有人在使用", 不是占座
    // (物品用 kItemClear: 不与人框重叠, 于是这里**只**在验证 C2)
    for (int i = 0; i < 20; ++i) {
        auto ev = step(100, {makeDet("book", kItemClear), makeDet("person", kPersonInZone)});
        EXPECT_TRUE(ev.empty()) << "人在座位上时不应判占座 (frame " << i << ")";
    }
    EXPECT_NE(an_->snapshot()[0].state, SeatState::Occupied);
}

TEST_F(SeatTest, PersonOutsideSeatDoesNotCount) {
    build();
    // 隔壁机位/远景的人, 不该影响这个座位
    auto ev0 =
        step(0, {makeDet("book", kItemClear), makeDet("person", cv::Rect(600, 100, 40, 100))});
    EXPECT_TRUE(ev0.empty());
    EXPECT_EQ(an_->snapshot()[0].state, SeatState::Pending);
}

TEST_F(SeatTest, MinPersonHeightFiltersFarPerson) {
    cfg_.min_person_height_px = 120; // 只认"够大"的人框
    cfg_.t_occupied_ms = 300;
    build();
    // 人框高 100 < 120 => 不算"有人", 于是物品独自留下 => 计时照走 => 正常告警
    std::vector<OccupancyEvent> all;
    for (int i = 0; i < 8; ++i) {
        for (auto& e : step(100, {makeDet("book", kItemClear), makeDet("person", kPersonInZone)})) {
            all.push_back(e);
        }
    }
    EXPECT_EQ(all.size(), 1u) << "过小的人框应被过滤, 不视为\"有人在用座位\"";
}

// C1b: 被同一帧里的人拿着的书, 不算"放在座位上"
TEST_F(SeatTest, ItemHeldByPersonIsNotOnSeat) {
    cfg_.person_seat_iou = 0.99f; // 关掉 C2 的"包含度"兜底 => 隔离验证 C1b
    build();
    const cv::Rect item(150, 180, 40, 30);
    const cv::Rect person(150, 170, 40, 60); // IoU=0.5, 且物品中心落在人框内 => 手持
    for (int i = 0; i < 20; ++i) {
        auto ev = step(100, {makeDet("book", item), makeDet("person", person)});
        EXPECT_TRUE(ev.empty()) << "拿在手里的书不应判成占座";
    }
    EXPECT_EQ(an_->snapshot()[0].item_count, 0);
}

// 对照组: 同样配置下, 物品不与人重叠(真放在桌上) => 正常计时
TEST_F(SeatTest, ItemNotHeldCountsNormally) {
    cfg_.person_seat_iou = 0.99f;
    build();
    const cv::Rect item(100, 180, 40, 30);   // 底边中点 (120,210) 在区内, 与人框无交集
    const cv::Rect person(150, 170, 40, 60);
    std::vector<OccupancyEvent> all;
    for (int i = 0; i < 20; ++i) {
        for (auto& e : step(100, {makeDet("book", item), makeDet("person", person)})) {
            all.push_back(e);
        }
    }
    EXPECT_EQ(all.size(), 1u) << "放在桌上(不与人重叠)应正常判占座";
    ASSERT_FALSE(all.empty()); // 先确认非空再索引, 否则失败时会越界(单测自己不该崩)
    EXPECT_EQ(all[0].seat, "A-12");
}

// ---------------------------------------------------------------------------
// C3: 持续时间阈值
// ---------------------------------------------------------------------------

TEST_F(SeatTest, OccupancyRequiresDwellThreshold) {
    build();
    const auto item = makeDet("book", kItemOnSeat);
    // t=0..900 (accum=900 < t_occupied=1000) 都还不该告警
    for (int i = 0; i < 10; ++i) {
        auto ev = step(100, {item});
        EXPECT_TRUE(ev.empty()) << "第 " << i << " 帧(累计 " << (i * 100) << "ms)不应判占座";
    }
    // t=1000 => accum=1000 => 占座
    auto ev = step(100, {item});
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].seat, "A-12");
    EXPECT_EQ(ev[0].evidence.state, SeatState::Occupied);
    EXPECT_GE(ev[0].evidence.dwell_ms, 1000);
    EXPECT_EQ(ev[0].evidence.item_count, 1);
}

TEST_F(SeatTest, ZeroThresholdFiresImmediately) {
    cfg_.t_occupied_ms = 0;
    build();
    auto ev = step(0, {makeDet("book", kItemOnSeat)});
    ASSERT_EQ(ev.size(), 1u); // t_occupied=0 => 条件成立当帧即告警
}

// C6: 上升沿 —— 一次占用只出一个事件, 不会每帧刷
TEST_F(SeatTest, EventFiresOncePerEpisode) {
    build();
    const auto item = makeDet("book", kItemOnSeat);
    int events = 0;
    for (int i = 0; i < 40; ++i) events += static_cast<int>(step(100, {item}).size());
    EXPECT_EQ(events, 1) << "持续占用 40 帧应只产生 1 个事件";

    // 物品被收走 => 解除; 再放回来 => 才允许再次告警
    step(100, {});
    events = 0;
    for (int i = 0; i < 20; ++i) events += static_cast<int>(step(100, {item}).size());
    EXPECT_EQ(events, 1) << "解除后重新占用应能再次告警";
}

// ---------------------------------------------------------------------------
// C4: 短暂离开容忍(暂停累计, 而非清零)
// ---------------------------------------------------------------------------

TEST_F(SeatTest, BriefPersonAppearancePausesNotResets) {
    build();
    // 物品用 kItemClear: 不与人框重叠 => 此处只验证 C4, 不会被 C1b 干扰
    const auto item = makeDet("book", kItemClear);
    for (int i = 0; i < 10; ++i) step(100, {item}); // accum = 900 (< 1000)
    ASSERT_EQ(an_->snapshot()[0].dwell_ms, 900);

    // 有人"路过"(被误检成人) 1 帧 => 只暂停, 不清零
    step(100, {item, makeDet("person", kPersonInZone)});
    EXPECT_EQ(an_->snapshot()[0].dwell_ms, 900) << "短暂出现应暂停累计而不是清零";

    // 人走了 => 从 900 继续累加, 100ms 后即达阈值
    auto ev = step(100, {item});
    ASSERT_EQ(ev.size(), 1u);
}

TEST_F(SeatTest, StablePersonResetsAccumulator) {
    build();
    const auto item = makeDet("book", kItemClear); // 同: 隔离 C1b
    const auto person = makeDet("person", kPersonInZone);
    for (int i = 0; i < 10; ++i) step(100, {item}); // 10 帧, accum = 900
    ASSERT_EQ(an_->snapshot()[0].dwell_ms, 900);

    // 人稳定在场 >= t_grace(300ms) => 真的"回来了" => 清零
    for (int i = 0; i < 4; ++i) step(100, {item, person}); // t=1100..1400
    EXPECT_EQ(an_->snapshot()[0].dwell_ms, 0) << "人稳定在场应清零累计";

    // 人走后必须重新攒满 t_occupied
    auto ev = step(100, {item});
    EXPECT_TRUE(ev.empty()) << "复位后不应立刻再告警";
}

// ---------------------------------------------------------------------------
// C5: M-of-N 稳定性投票(单帧漏检不该把计时打回原点)
// ---------------------------------------------------------------------------

TEST_F(SeatTest, VotingAbsorbsSingleFrameDropout) {
    cfg_.vote_n = 10;
    cfg_.vote_m = 7;
    build();
    const auto item = makeDet("book", kItemOnSeat);
    for (int i = 0; i < 9; ++i) step(100, {item}); // t=100..900, accum = 800
    ASSERT_EQ(an_->snapshot()[0].dwell_ms, 800);

    // 单帧漏检(某帧没检出书): 10 帧窗口里仍有 9 帧命中 >= 7 => 不重置
    auto ev = step(100, {});
    EXPECT_TRUE(ev.empty());
    EXPECT_GE(an_->snapshot()[0].dwell_ms, 800) << "投票窗口应吸收单帧漏检, 不清零";

    // 再一帧即越过阈值 => 仍能正常告警
    ev = step(100, {item});
    ASSERT_EQ(ev.size(), 1u);
}

TEST_F(SeatTest, WithoutVotingDropoutResets) {
    // 对照组: vote_n=1(不投票) => 同样的单帧漏检会把计时清零
    build();
    const auto item = makeDet("book", kItemOnSeat);
    for (int i = 0; i < 10; ++i) step(100, {item}); // 10 帧, accum = 900

    step(100, {}); // 单帧漏检 => 清零
    EXPECT_EQ(an_->snapshot()[0].dwell_ms, 0);

    // 从零重来: 还要再攒 10 帧才够
    std::vector<OccupancyEvent> all;
    for (int i = 0; i < 9; ++i) {
        for (auto& e : step(100, {item})) all.push_back(e);
    }
    EXPECT_TRUE(all.empty()) << "清零后不足阈值不应告警";
}

TEST_F(SeatTest, VoteMajoritySuppressesFlapping) {
    cfg_.vote_n = 4;
    cfg_.vote_m = 4; // 要求最近 4 帧全部命中
    build();
    const auto item = makeDet("book", kItemOnSeat);
    // 奇偶抖动: 书一闪一闪 => 任一 4 帧窗口里都不可能有 4 帧命中
    int events = 0;
    for (int i = 0; i < 40; ++i) {
        events += static_cast<int>(step(100, (i % 2 == 0) ? std::vector<DetectionResult>{item}
                                                          : std::vector<DetectionResult>{}).size());
    }
    EXPECT_EQ(events, 0) << "抖动序列不应判占座";
}

// ---------------------------------------------------------------------------
// C7: 证据文案
// ---------------------------------------------------------------------------

TEST_F(SeatTest, EvidenceDescribesSituation) {
    cfg_.t_occupied_ms = 200;
    build();
    auto ev = step(0, {makeDet("book", kItemOnSeat)});
    ev = step(300, {makeDet("book", kItemOnSeat)});
    ASSERT_EQ(ev.size(), 1u);
    ASSERT_FALSE(ev.empty());
    const std::string text = ev[0].evidence.describe();
    EXPECT_NE(text.find("A-12"), std::string::npos);
    EXPECT_NE(text.find("book"), std::string::npos);
    EXPECT_NE(text.find("无人使用"), std::string::npos); // 证据里要有人能读懂的口径
}

TEST_F(SeatTest, EvidenceConfidenceIsVoteRatio) {
    cfg_.vote_n = 10;
    cfg_.vote_m = 7;
    cfg_.t_occupied_ms = 100;
    build();
    const auto item = makeDet("book", kItemOnSeat);
    std::vector<OccupancyEvent> all;
    for (int i = 0; i < 10; ++i) {
        for (auto& e : step(100, {item})) all.push_back(e);
    }
    ASSERT_EQ(all.size(), 1u);
    ASSERT_FALSE(all.empty());
    // 10 帧全命中 => 置信度(证据一致率) = 1.0
    EXPECT_NEAR(all[0].evidence.confidence(), 1.0f, 1e-4);
}

TEST(SeatEvidenceText, IdleDescribesNoItems) {
    occupancy::SeatEvidence ev;
    ev.seat = "B-01";
    ev.state = SeatState::Idle;
    const std::string text = ev.describe();
    EXPECT_NE(text.find("B-01"), std::string::npos);
    EXPECT_NE(text.find("未检出物品"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 生命周期 / 边界
// ---------------------------------------------------------------------------

TEST_F(SeatTest, DisabledAnalyzerNeverFires) {
    cfg_.enabled = false;
    build();
    EXPECT_FALSE(an_->enabled());
    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(step(100, {makeDet("book", kItemOnSeat)}).empty());
    }
    EXPECT_EQ(an_->stats().events, 0u);
}

TEST_F(SeatTest, NoSeatsNeverFires) {
    zones_.clear();
    build();
    EXPECT_FALSE(an_->enabled());
    EXPECT_TRUE(step(100, {makeDet("book", kItemOnSeat)}).empty());
}

TEST_F(SeatTest, MultipleSeatsAreIndependent) {
    cfg_.t_occupied_ms = 500;
    zones_ = {zoneRect("S1", 100, 100, 120, 120), zoneRect("S2", 400, 400, 120, 120)};
    build();
    // 只有 S1 有物品
    std::vector<OccupancyEvent> all;
    for (int i = 0; i < 10; ++i) {
        for (auto& e : step(100, {makeDet("book", cv::Rect(150, 180, 40, 30))})) {
            all.push_back(e);
        }
    }
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].seat, "S1");
    // S2 始终空闲
    ASSERT_EQ(an_->snapshot().size(), 2u);
    EXPECT_EQ(an_->snapshot()[0].state, SeatState::Occupied);
    EXPECT_EQ(an_->snapshot()[1].state, SeatState::Idle);
}

TEST_F(SeatTest, StatsAndSnapshotConsistent) {
    build();
    const auto item = makeDet("book", kItemOnSeat);
    for (int i = 0; i < 20; ++i) step(100, {item});
    const auto st = an_->stats();
    EXPECT_EQ(st.frames, 20u);
    EXPECT_EQ(st.events, 1u);
    EXPECT_EQ(st.seats, 1u);
    ASSERT_EQ(an_->snapshot().size(), 1u);
    EXPECT_TRUE(an_->snapshot()[0].occupied());
}

TEST_F(SeatTest, OutOfOrderTimestampDoesNotAccumulateNegatively) {
    build();
    const auto item = makeDet("book", kItemOnSeat);
    step(100, {item});
    step(100, {item});
    step(100, {item});
    const std::int64_t before = an_->snapshot()[0].dwell_ms;
    ASSERT_GT(before, 0);
    // 时间倒流(理论上不该发生): 不能出现负增量, 更不能把计时打回负数
    t_ -= 500;
    an_->update({item}, t_);
    EXPECT_GE(an_->snapshot()[0].dwell_ms, before);
}

TEST(SeatZone, BoundingRect) {
    const auto z = zoneRect("Z", 10, 20, 100, 50);
    const cv::Rect r = z.boundingRect();
    EXPECT_EQ(r.x, 10);
    EXPECT_EQ(r.y, 20);
    EXPECT_EQ(r.width, 100);
    EXPECT_EQ(r.height, 50);
    EXPECT_EQ(occupancy::SeatZone{}.boundingRect(), cv::Rect());
}

TEST(SeatState, ToString) {
    EXPECT_STREQ(occupancy::seatStateToString(SeatState::Idle), "idle");
    EXPECT_STREQ(occupancy::seatStateToString(SeatState::Pending), "pending");
    EXPECT_STREQ(occupancy::seatStateToString(SeatState::Occupied), "occupied");
}

TEST(SeatConfig, DefaultItemLabelsDoNotContainPerson) {
    const auto& labels = occupancy::defaultItemLabels();
    EXPECT_FALSE(labels.empty());
    EXPECT_EQ(std::find(labels.begin(), labels.end(), std::string("person")), labels.end());
}

TEST(SeatGeometry, ItemHeldByPersonUsesContainmentNotIou) {
    // 真人框远大于一本书: IoU 必然很小, 若用 IoU 就会漏判"拿在手上" => 必须用包含度。
    // 这里不构造状态机, 只验证度量选择本身(拿现实尺寸的比例做断言)。
    const cv::Rect person(100, 100, 200, 400); // 人
    const cv::Rect book(150, 150, 60, 45);     // 书(完全落在人框内, 即"拿在手里")
    const float inter = static_cast<float>(book.area());
    const float iou = inter / (static_cast<float>(person.area()) + inter - inter);
    EXPECT_LT(iou, 0.1f) << "IoU 在此场景天然很小 => 不能用它判手持";
    // 包含度 = 书在人框内的比例 = 1.0
    EXPECT_NEAR(static_cast<float>((book & person).area()) / static_cast<float>(book.area()), 1.0f,
                1e-4);
}

} // namespace
