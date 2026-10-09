// 标定探针单测 (tests/unit/test_seat_probe.cpp)
//
// SeatProbe 是纯逻辑: 输入(画面尺寸 + 检测框) -> 输出(网格热力 + 建议 zone)。
// 于是"物品该被算进哪一格""热格合并出的建议矩形对不对"这类**标定口径**问题,
// 可以在这里用确定性用例锁死, 而不必真的开机位跑一遍视频。
//
// 共用约定 (与占座判定同源):
//   归属点 = 框**底边中点** (bottom-center), 不是框中心。
//   本文件网格: 400x400 / 4x4 => 每格 100x100, index = row*4 + col。
#include "calibration/SeatProbe.h"

#include <vector>

#include <gtest/gtest.h>

namespace {

using calibration::SeatProbe;

DetectionResult makeDet(const std::string& label, const cv::Rect& box, float conf = 0.9f) {
    DetectionResult d;
    d.class_id = 0;
    d.confidence = conf;
    d.box = box;
    d.label = label;
    return d;
}

SeatProbe::Config cfg4x4(int min_item_hits = 1) {
    SeatProbe::Config c;
    c.grid_cols = 4;
    c.grid_rows = 4;
    c.min_item_hits = min_item_hits;
    return c;
}

SeatProbe makeProbe(int w = 400, int h = 400, int min_item_hits = 1) {
    return SeatProbe(cfg4x4(min_item_hits), w, h, {"book", "bag", "laptop"}, "person");
}

// 空输入: 无任何命中 => 不应给出建议(免得商家拿到一个凭空捏造的 zone)
TEST(SeatProbe, EmptyFramesYieldNoSuggestion) {
    SeatProbe p = makeProbe();
    const auto rep = p.report();
    EXPECT_FALSE(rep.suggestion_valid);
    EXPECT_EQ(rep.cells.size(), 16u);
    for (const auto& c : rep.cells) {
        EXPECT_EQ(c.item_hits, 0);
        EXPECT_EQ(c.person_hits, 0);
    }
}

// 归属点用底边中点: 框中心在 r0c2, 底边中点落在 r1c2 => 应记到 r1c2 (index 6)
TEST(SeatProbe, UsesBottomCenterNotBoxCenter) {
    SeatProbe p = makeProbe();
    // box=(240,20,40,90): 中心(260,65)->r0c2; 底边中点(260,110)->r1c2
    EXPECT_EQ(p.cellIndex(cv::Rect(240, 20, 40, 90)), 6);
    p.addFrame({makeDet("book", cv::Rect(240, 20, 40, 90))});
    const auto rep = p.report();
    EXPECT_EQ(rep.cells[6].item_hits, 1);
    EXPECT_EQ(rep.cells[2].item_hits, 0); // 若误用框中心, 这里会是 1
}

// 单词命中 -> 建议矩形正好覆盖那一格
TEST(SeatProbe, SingleHitSuggestsThatCell) {
    SeatProbe p = makeProbe();
    // 底边中点 (250,150) => r1c2 (index 6)
    p.addFrame({makeDet("bag", cv::Rect(230, 100, 40, 50))});
    const auto rep = p.report();
    ASSERT_TRUE(rep.suggestion_valid);
    EXPECT_EQ(rep.suggested_zone, cv::Rect(200, 100, 100, 100));
}

// 底边中点落出画面 => 不归属任何格
TEST(SeatProbe, OutOfFrameBottomCenterIsIgnored) {
    SeatProbe p = makeProbe();
    EXPECT_EQ(p.cellIndex(cv::Rect(-100, 20, 40, 50)), -1);  // 底边中点 x=-80
    EXPECT_EQ(p.cellIndex(cv::Rect(380, 20, 40, 50)), -1);   // 底边中点 x=400 (>= 宽度)
    EXPECT_EQ(p.cellIndex(cv::Rect(100, -80, 40, 50)), -1);  // 底边中点 y=-30
    p.addFrame({makeDet("book", cv::Rect(-100, 20, 40, 50)),
                makeDet("book", cv::Rect(380, 20, 40, 50))});
    const auto rep = p.report();
    for (const auto& c : rep.cells) EXPECT_EQ(c.item_hits, 0);
    EXPECT_FALSE(rep.suggestion_valid);
}

// person 只进 person_hits; 非 item/person 类别被忽略
TEST(SeatProbe, LabelRoutingSeparatesItemPersonAndIgnoresOthers) {
    SeatProbe p = makeProbe();
    p.addFrame({makeDet("person", cv::Rect(230, 60, 40, 90)), // 底边中点(250,150)->r1c2
                makeDet("chair", cv::Rect(230, 100, 40, 50)),  // 无关类别
                makeDet("book", cv::Rect(230, 100, 40, 50))}); // 底边中点(250,150)->r1c2
    const auto rep = p.report();
    EXPECT_EQ(rep.cells[6].item_hits, 1);   // 只有 book
    EXPECT_EQ(rep.cells[6].person_hits, 1); // person 独立计数
    EXPECT_EQ(rep.cells[2].item_hits, 0);
}

// min_item_hits 去噪: 只出现一次的热格不该撑起建议
TEST(SeatProbe, MinItemHitsFiltersFlukes) {
    // 噪声: r0c0 命中 2 次(真热点); r1c1 命中 1 次(偶发)
    SeatProbe noise_sup = makeProbe(400, 400, /*min_item_hits=*/2);
    noise_sup.addFrame({makeDet("book", cv::Rect(10, 10, 20, 20)),
                        makeDet("book", cv::Rect(10, 10, 20, 20))}); // r0c0
    noise_sup.addFrame({makeDet("book", cv::Rect(110, 110, 20, 20))}); // r1c1 (仅 1 次)
    const auto rep_sup = noise_sup.report();
    ASSERT_TRUE(rep_sup.suggestion_valid);
    EXPECT_EQ(rep_sup.suggested_zone, cv::Rect(0, 0, 100, 100)); // 只圈 r0c0

    // 同数据、min_item_hits=1: 两格都算 => 外接矩形跨 r0c0..r1c1
    SeatProbe keep_all = makeProbe(400, 400, /*min_item_hits=*/1);
    keep_all.addFrame({makeDet("book", cv::Rect(10, 10, 20, 20)),
                       makeDet("book", cv::Rect(10, 10, 20, 20))});
    keep_all.addFrame({makeDet("book", cv::Rect(110, 110, 20, 20))});
    const auto rep_all = keep_all.report();
    ASSERT_TRUE(rep_all.suggestion_valid);
    EXPECT_EQ(rep_all.suggested_zone, cv::Rect(0, 0, 200, 200));
}

// 非整除尺寸: 每格向上取整, 右/下边缘的目标仍能落进最后一格, 且建议矩形被裁回画面内
TEST(SeatProbe, NonDivisibleSizeClampsToFrame) {
    SeatProbe p = SeatProbe(cfg4x4(1), 450, 450, {"book"}, "person");
    EXPECT_EQ(p.report().cell_w, 113); // ceil(450/4)
    EXPECT_EQ(p.cellIndex(cv::Rect(430, 420, 20, 20)), 15); // 底边中点(440,440) -> r3c3
    p.addFrame({makeDet("book", cv::Rect(430, 420, 20, 20))});
    const auto rep = p.report();
    ASSERT_TRUE(rep.suggestion_valid);
    // 建议矩形 = r3c3 => (339,339,113,113), 裁剪回 450x450 后宽高各 111
    EXPECT_EQ(rep.suggested_zone, cv::Rect(339, 339, 111, 111));
}

// 边界值: x=99 -> col0; x=100 -> col1; 底边中点 y=400 越界
TEST(SeatProbe, CellBoundarySemantics) {
    SeatProbe q = makeProbe();
    // 底边中点(99,200) -> col0 row2 => index 8
    EXPECT_EQ(q.cellIndex(cv::Rect(79, 199, 40, 1)), 8);
    // 底边中点(100,1) -> col1 row0 => index 1
    EXPECT_EQ(q.cellIndex(cv::Rect(80, 0, 40, 1)), 1);
    // 底边中点(100,400) 越界
    EXPECT_EQ(q.cellIndex(cv::Rect(80, 399, 40, 1)), -1);
}

// 报告文本: 有建议 / 无建议 两种文案都要可读
TEST(SeatProbe, DescribeMentionsSuggestionOrLackThereof) {
    SeatProbe empty = makeProbe();
    EXPECT_NE(empty.report().describe().find("无法给出建议"), std::string::npos);

    SeatProbe p = makeProbe();
    p.addFrame({makeDet("book", cv::Rect(230, 100, 40, 50))});
    const std::string text = p.report().describe();
    EXPECT_NE(text.find("建议座位区"), std::string::npos);
    EXPECT_NE(text.find("200"), std::string::npos); // 建议 x 坐标
}

// 标签分布记录**所有**类别(含无关类别), 次数降序、同次数按名字升序
TEST(SeatProbe, LabelHistogramRecordsEveryLabel) {
    SeatProbe p = makeProbe();
    p.addFrame({makeDet("book", cv::Rect(230, 100, 40, 50)),  // item
                makeDet("chair", cv::Rect(10, 10, 20, 20)),   // 无关类别, 仍要计数
                makeDet("chair", cv::Rect(10, 10, 20, 20)),
                makeDet("person", cv::Rect(230, 60, 40, 90))});
    const auto rep = p.report();
    ASSERT_EQ(rep.label_top.size(), 3u);
    EXPECT_EQ(rep.label_top[0].first, "chair"); // 2 次, 最多
    EXPECT_EQ(rep.label_top[0].second, 2);
    // book=1 与 person=1 同次数 => 按名字升序 => book 在前
    EXPECT_EQ(rep.label_top[1].first, "book");
    EXPECT_EQ(rep.label_top[2].first, "person");
    EXPECT_NE(rep.describe().find("chair=2"), std::string::npos);
}

// JSON 输出: 字段口径必须与 describe() 一致(web 端热力图直接吃这份数据)。
// 全整数输出 => 可精确断言, 不受浮点/locale 影响。
TEST(SeatProbe, ToJsonMatchesReportExactly) {
    SeatProbe p = makeProbe();          // 400x400 / 4x4 => 每格 100x100
    // 底边中点(250,150) -> r1c2 (index 6): book 计 1; person 计 1
    p.addFrame({makeDet("book", cv::Rect(230, 100, 40, 50)),
                makeDet("person", cv::Rect(230, 60, 40, 90)),
                makeDet("chair", cv::Rect(10, 10, 20, 20))}); // 无关类别, 只进 label_top

    const std::string json = p.report().toJson();
    EXPECT_EQ(json,
              "{\"width\":400,\"height\":400,\"cols\":4,\"rows\":4,"
              "\"cell_w\":100,\"cell_h\":100,"
              "\"cells\":["
              "{\"row\":0,\"col\":0,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":0,\"col\":1,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":0,\"col\":2,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":0,\"col\":3,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":1,\"col\":0,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":1,\"col\":1,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":1,\"col\":2,\"item_hits\":1,\"person_hits\":1},"
              "{\"row\":1,\"col\":3,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":2,\"col\":0,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":2,\"col\":1,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":2,\"col\":2,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":2,\"col\":3,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":3,\"col\":0,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":3,\"col\":1,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":3,\"col\":2,\"item_hits\":0,\"person_hits\":0},"
              "{\"row\":3,\"col\":3,\"item_hits\":0,\"person_hits\":0}"
              "],"
              "\"suggestion_valid\":true,"
              "\"suggested_zone\":{\"x\":200,\"y\":100,\"width\":100,\"height\":100},"
              "\"label_top\":[{\"label\":\"book\",\"count\":1},"
              "{\"label\":\"chair\",\"count\":1},{\"label\":\"person\",\"count\":1}]"
              "}");
}

// 无建议时 suggested_zone 必须是 null(不是 {} 或缺失) —— 前端据此判断"还没法给建议"
TEST(SeatProbe, ToJsonEmitsNullZoneWhenNoSuggestion) {
    SeatProbe p = makeProbe();
    const std::string json = p.report().toJson();
    EXPECT_NE(json.find("\"suggestion_valid\":false"), std::string::npos);
    EXPECT_NE(json.find("\"suggested_zone\":null"), std::string::npos);
    EXPECT_EQ(json.find("\"suggested_zone\":{"), std::string::npos);
    EXPECT_NE(json.find("\"label_top\":[]"), std::string::npos);
}

} // namespace
