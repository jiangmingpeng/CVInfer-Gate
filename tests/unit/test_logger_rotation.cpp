#include "utils/Logger.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

// ============================================================
// 日志文件轮转单测 (T43)
// ------------------------------------------------------------
// 为什么必须测: 轮转写错 = 日志要么无限涨(撑爆磁盘), 要么被静默清空(审计断档)。
// 覆盖:
//   * max_size_mb=0 => 不轮转(默认行为不变)
//   * 写入超过阈值 => 出现 .1, 当前文件重新开始
//   * keep_files 上限生效(不会无限增生 .2/.3/...)
//   * keep_files=0 => 只留当前文件
// 测试期间把 std::cout 重定向到 /dev/null: Logger 设计上双写控制台,
//   否则几 MB 日志会把 CI 输出刷爆。
// ============================================================

namespace fs = std::filesystem;

namespace {

class LoggerRotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("cvinfer_log_test_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        path_ = (dir_ / "app.log").string();
        sink_.open("/dev/null");
        saved_cout_ = std::cout.rdbuf(sink_.rdbuf());   // 静音
    }

    void TearDown() override {
        std::cout.rdbuf(saved_cout_);
        // 还原为"仅控制台", 避免影响同一进程内的其它测试(Logger 是单例)
        LogConfig console_only;
        console_only.level = "info";
        Logger::instance().init(console_only);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    LogConfig makeCfg(int max_size_mb, int keep_files) {
        LogConfig cfg;
        cfg.level = "info";
        cfg.file = path_;
        cfg.max_size_mb = max_size_mb;
        cfg.keep_files = keep_files;
        return cfg;
    }

    // 写够 ~2MB(1MB 阈值必被触发)
    void writeBulk(char filler) {
        const std::string pad(160, filler);
        for (int i = 0; i < 12000; ++i) CVLOG_INFO << pad << ' ' << i;
    }

    fs::path dir_;
    std::string path_;
    std::ofstream sink_;
    std::streambuf* saved_cout_ = nullptr;
};

}  // namespace

TEST_F(LoggerRotationTest, DisabledByDefault) {
    Logger::instance().init(makeCfg(/*max_size_mb=*/0, /*keep_files=*/3));
    for (int i = 0; i < 200; ++i) CVLOG_INFO << "line " << i;

    EXPECT_EQ(Logger::instance().rotations(), 0u);
    EXPECT_FALSE(fs::exists(path_ + ".1"));
    EXPECT_TRUE(fs::exists(path_));
}

TEST_F(LoggerRotationTest, RotatesWhenExceedingMaxSize) {
    Logger::instance().init(makeCfg(1, 2));
    const std::uint64_t size_before = fs::file_size(path_);   // 默认 0(新文件)

    writeBulk('x');

    EXPECT_GE(Logger::instance().rotations(), 1u);
    EXPECT_TRUE(fs::exists(path_ + ".1"));
    EXPECT_TRUE(fs::exists(path_));
    EXPECT_FALSE(fs::exists(path_ + ".3"));   // keep_files=2 => 不会增生到 .3
    EXPECT_GT(sink_.is_open() ? size_before + 1 : 0, 0u);
    // 轮转后重新计数 => 当前文件不该是一个"几十 MB"的巨物
    EXPECT_LE(fs::file_size(path_), 4ull * 1024 * 1024);
}

TEST_F(LoggerRotationTest, DoesNotRotateBelowThreshold) {
    Logger::instance().init(makeCfg(1, 2));
    CVLOG_INFO << "small line";
    const std::uint64_t before = Logger::instance().rotations();
    CVLOG_INFO << "another small line";
    EXPECT_EQ(Logger::instance().rotations(), before);
}

TEST_F(LoggerRotationTest, KeepFilesZeroDiscardsArchive) {
    Logger::instance().init(makeCfg(1, 0));
    writeBulk('y');

    EXPECT_GE(Logger::instance().rotations(), 1u);
    EXPECT_TRUE(fs::exists(path_));            // 当前文件在
    EXPECT_FALSE(fs::exists(path_ + ".1"));    // 不留归档
}

TEST_F(LoggerRotationTest, ExistingFileSizeCountsTowardBudget) {
    // 续写已有文件时, 已有大小必须算进预算, 否则重启后要再写满一个整额才轮转
    {
        std::ofstream pre(path_, std::ios::binary | std::ios::trunc);
        const std::string big(1500 * 1024, 'z');
        pre << big;
    }
    Logger::instance().init(makeCfg(1, 2));
    CVLOG_INFO << "one more line";   // 已有 1.5MB + 这一行 => 立刻越界
    EXPECT_GE(Logger::instance().rotations(), 1u);
    EXPECT_TRUE(fs::exists(path_ + ".1"));
}
