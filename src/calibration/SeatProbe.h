#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "inference/DetectionResult.h"

// SeatProbe (标定探针: 把检测框聚合成"物品落区热力 + 建议座位 zone")
//
// ---------------------------------------------------------------------------
// 为什么需要这一层
// ---------------------------------------------------------------------------
// occupancy.seats 是一组**静态标定常量**(每个现场都不一样), 它绑定了
// 「分辨率 + 机位 + 构图」—— 换个商家/换个镜头, 旧坐标即作废。问题在于:
// 让商家"手改 YAML 里的像素坐标"几乎不可能对准, 于是 zone 画偏、判定失灵,
// 却往往要上线跑半天才发现(而且越界警告只拦"框出画面", 拦不住"框错地方")。
//
// 这一层把"定位物品实际放在哪"变成一次**可自助测量的动作**: 跑一段真实视频,
// 把 item_labels 目标的**底边中点**(与占座判定 C1 判据同一口径)落进网格,
// 统计热力, 再合并热格给出一个**建议 rect**。商家照此填/直接采纳, zone 就对上了。
//
// 关键约定(别改坏):
//   * 归属点 = 框**底边中点**(bottom-center), 不是框中心 —— 与
//     occupancy::SeatOccupancyAnalyzer 的 C1 判据保持一致, 否则会出现
//     "探针说物品在这格、判定却抓不住"的口径漂移。
//   * 纯逻辑: 不碰视频/模型/数据库。输入是"画面尺寸 + 一帧帧检测结果",
//     因此可给出确定性断言, 能被单测锁住。
// ---------------------------------------------------------------------------

namespace calibration {

class SeatProbe {
public:
    // 网格粒度与去噪阈值。默认 4x4 网格足以让商家肉眼定位;
    // min_item_hits 用来滤掉偶发误检(只出现一次的目标不该撑起一个 zone)。
    struct Config {
        int grid_cols = 4;
        int grid_rows = 4;
        int min_item_hits = 1;
    };

    // 一个网格单元的累计命中(行优先存储: index = row * cols + col)
    struct Cell {
        int row = 0;
        int col = 0;
        std::int64_t item_hits = 0;   // item 目标底边中点落格次数
        std::int64_t person_hits = 0; // person 目标底边中点落格次数
    };

    // 探针报告: 每格热力 + 建议座位区
    struct Report {
        int width = 0;
        int height = 0;
        int cols = 0;
        int rows = 0;
        int cell_w = 0; // 每格像素宽(向上取整, 保证覆盖整幅画面)
        int cell_h = 0;
        std::vector<Cell> cells; // rows * cols, 行优先
        cv::Rect suggested_zone; // item 热格的最小外接矩形(裁剪到画面内)
        bool suggestion_valid = false;

        // 标签分布(所有类别, 次数降序, 最多 12 条):
        // 让商家一眼看到"我的物品标签到底有没有被检出" —— 若 book/laptop 始终为 0,
        // 那问题不在 zone 画错, 而在模型/标签/场景本身就抓不到, 白改坐标也没用。
        std::vector<std::pair<std::string, std::int64_t>> label_top;

        // 人可读报告(可直接贴给商家, 也可供上层日志/接口输出)
        std::string describe() const;

        // 机器可读报告: 单行 JSON, 字段口径与 describe() **完全一致**。
        //
        // 存在的意义(别绕过): 网页热力图是"画"出来的, 但"哪个框算进哪一格、每格算几次"
        // 必须由这里算完 —— Python 只做渲染, 不做重算。否则会出现
        // 「线上判定用 C++ 口径、网页热力图用 Python 口径」的分叉, 热力图就骗人了。
        // 无第三方 JSON 依赖: 手写输出(全整数, 无浮点 => 不受 locale 影响, 可精确断言)。
        std::string toJson() const;
    };

    // width/height = 本视频画面尺寸。探针针对"一段固定尺寸的视频",
    // 故尺寸在构造期确定, 后续 addFrame 只喂检测结果。
    SeatProbe(Config cfg, int width, int height,
              std::vector<std::string> item_labels, std::string person_label);

    // 逐帧喂入检测结果(线程约束: 单线程调用, 内部只做累加)
    void addFrame(const std::vector<DetectionResult>& dets);

    // 汇总报告(可重复调用, 不改变内部状态)
    Report report() const;

    // 单测钩子: 一个框底边中点所在格子下标; 落在画面外/边界异常时返回 -1
    int cellIndex(const cv::Rect& box) const;

    int itemGridSize() const { return static_cast<int>(item_grid_.size()); }

private:
    Config cfg_;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::string> item_labels_;
    std::string person_label_;
    std::vector<std::int64_t> item_grid_;   // rows * cols
    std::vector<std::int64_t> person_grid_; // rows * cols
    std::map<std::string, std::int64_t> label_counts_; // 所有标签的累计次数
};

} // namespace calibration
