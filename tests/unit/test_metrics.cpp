#include "utils/Metrics.h"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

// Metrics 注册表单测
// 覆盖的都是"线上会咬人"的地方:
// * 同名不同标签 = 独立样本(否则 gRPC 分方法/结果码的计数会互相污染)
// * "推"(inc) 与 "拉"(collector)混用: 拉取是**抓取时求值**, 不是建册时快照
// * 拉式指标被 inc 时不得被污染(避免两套数据打架)
// * # HELP 在同一指标名下只出现一次, 且不因"先遇到的样本没带 help"而丢失
// * 计数输出整数、仪表输出小数(格式不能反)
// * 采集器抛异常不能把整个 /metrics 打挂

namespace {

using metrics::Registry;
using metrics::Type;

class MetricsTest : public ::testing::Test {
protected:
    void SetUp() override { Registry::instance().clear(); }
    void TearDown() override { Registry::instance().clear(); }
};

} // namespace

TEST_F(MetricsTest, CounterGroupsByLabels) {
    Registry& reg = Registry::instance();
    reg.inc("cvinfer_test_total");
    reg.inc("cvinfer_test_total", 4);
    reg.inc("cvinfer_test_total", "code=\"OK\"");

    const std::string out = reg.render();
    EXPECT_NE(out.find("# TYPE cvinfer_test_total counter"), std::string::npos);
    EXPECT_NE(out.find("cvinfer_test_total 5\n"), std::string::npos); // 计数器 = 整数
    EXPECT_NE(out.find("cvinfer_test_total{code=\"OK\"} 1\n"), std::string::npos); // 标签独立计数
    EXPECT_EQ(reg.size(), 2u);
}

TEST_F(MetricsTest, GaugeHasHelpAndDecimalValue) {
    Registry& reg = Registry::instance();
    reg.declare("cvinfer_test_gauge", Type::Gauge, "测试用仪表");
    reg.setGauge("cvinfer_test_gauge", 3.5);

    const std::string out = reg.render();
    EXPECT_NE(out.find("# HELP cvinfer_test_gauge 测试用仪表"), std::string::npos);
    EXPECT_NE(out.find("# TYPE cvinfer_test_gauge gauge"), std::string::npos);
    EXPECT_NE(out.find("cvinfer_test_gauge 3.5\n"), std::string::npos);
}

TEST_F(MetricsTest, CollectorsArePulledAtScrapeTime) {
    Registry& reg = Registry::instance();
    int live = 7;
    reg.addCollector("cvinfer_test_pull", Type::Counter, "测试用拉取", [&live] { return live; });

    EXPECT_NE(reg.render().find("cvinfer_test_pull 7\n"), std::string::npos);
    live = 42; // 抓取时现算 => 立刻反映
    EXPECT_NE(reg.render().find("cvinfer_test_pull 42\n"), std::string::npos);

    // 拉式指标不接受手推: 否则会出现"两套数据打架"
    reg.inc("cvinfer_test_pull", 100);
    EXPECT_NE(reg.render().find("cvinfer_test_pull 42\n"), std::string::npos);
}

TEST_F(MetricsTest, HelpPrintedOncePerNameEvenIfFirstSampleHasNone) {
    Registry& reg = Registry::instance();
    reg.declare("cvinfer_test_multi", Type::Counter, "多标签指标", "b=\"2\"");
    reg.inc("cvinfer_test_multi", "a=\"1\""); // 排序后 a 居前(它没有 help)

    const std::string out = reg.render();
    const std::size_t first = out.find("# HELP cvinfer_test_multi");
    ASSERT_NE(first, std::string::npos);
    EXPECT_EQ(out.find("# HELP cvinfer_test_multi", first + 1), std::string::npos); // 只有一条
    EXPECT_NE(out.find("cvinfer_test_multi{a=\"1\"} 1\n"), std::string::npos);
    EXPECT_NE(out.find("cvinfer_test_multi{b=\"2\"} 0\n"), std::string::npos);
}

TEST_F(MetricsTest, ThrowingCollectorDoesNotBreakScrape) {
    Registry& reg = Registry::instance();
    reg.addCollector("cvinfer_test_throw", Type::Gauge, "会抛异常的采集器",
                     []() -> double { throw std::runtime_error("boom"); });

    const std::string out = reg.render();
    EXPECT_NE(out.find("cvinfer_test_throw 0\n"), std::string::npos);
}

TEST_F(MetricsTest, ClearResetsRegistry) {
    Registry& reg = Registry::instance();
    reg.inc("cvinfer_test_total");
    ASSERT_EQ(reg.size(), 1u);
    reg.clear();
    EXPECT_EQ(reg.size(), 0u);
    EXPECT_TRUE(reg.render().empty());
}
