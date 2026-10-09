#include "calibration/SeatProbe.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace calibration {

namespace {

// 框底边中点 = 探针与占座判定共享的归属点(见头文件"关键约定")
cv::Point2f bottomCenter(const cv::Rect& r) {
    return cv::Point2f(static_cast<float>(r.x) + static_cast<float>(r.width) * 0.5f,
                       static_cast<float>(r.y + r.height));
}

// 每格像素宽/高: 向上取整, 保证最后一格也能覆盖到画面右/下边缘
int cellSpan(int total, int cells) {
    if (cells < 1) return total > 0 ? total : 1;
    return (total + cells - 1) / cells;
}

// JSON 字符串转义(标签名来自模型, 理论上是简单标识符; 仍按 RFC8259 把控制字符/引号/
// 反斜杠转义, 免得一个意外字符就让整段 JSON 解析失败)。
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (u < 0x20) {
                    // 其余控制字符 -> \u00XX
                    static const char* kHex = "0123456789abcdef";
                    out += "\\u00";
                    out += kHex[(u >> 4) & 0xF];
                    out += kHex[u & 0xF];
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

} // namespace

SeatProbe::SeatProbe(Config cfg, int width, int height,
                     std::vector<std::string> item_labels, std::string person_label)
    : cfg_(std::move(cfg)),
      width_(width),
      height_(height),
      item_labels_(std::move(item_labels)),
      person_label_(std::move(person_label)) {
    if (cfg_.grid_cols < 1) cfg_.grid_cols = 1;
    if (cfg_.grid_rows < 1) cfg_.grid_rows = 1;
    if (cfg_.min_item_hits < 0) cfg_.min_item_hits = 0;
    const std::size_t n = static_cast<std::size_t>(cfg_.grid_cols) * cfg_.grid_rows;
    item_grid_.assign(n, 0);
    person_grid_.assign(n, 0);
}

int SeatProbe::cellIndex(const cv::Rect& box) const {
    if (width_ <= 0 || height_ <= 0) return -1;
    const cv::Point2f bc = bottomCenter(box);
    // 底边中点落出画面 => 不归属任何格(负坐标/超界都不计, 避免把画面外目标算进来)
    if (bc.x < 0.0f || bc.y < 0.0f ||
        bc.x >= static_cast<float>(width_) || bc.y >= static_cast<float>(height_)) {
        return -1;
    }
    const int cw = cellSpan(width_, cfg_.grid_cols);
    const int ch = cellSpan(height_, cfg_.grid_rows);
    const int col = static_cast<int>(bc.x) / cw;
    const int row = static_cast<int>(bc.y) / ch;
    // 理论上 col/row 已被范围排除在 [0, cols/h - 1], 这里再夹一次防整数边界意外
    const int colc = std::min(col, cfg_.grid_cols - 1);
    const int rowc = std::min(row, cfg_.grid_rows - 1);
    return rowc * cfg_.grid_cols + colc;
}

void SeatProbe::addFrame(const std::vector<DetectionResult>& dets) {
    for (const auto& d : dets) {
        // 标签分布记录**所有**类别(含无关类别): 商家据此判断"物品标签是否被抓到"
        ++label_counts_[d.label];
        const bool is_item =
            std::find(item_labels_.begin(), item_labels_.end(), d.label) != item_labels_.end();
        const bool is_person = (!person_label_.empty() && d.label == person_label_);
        if (!is_item && !is_person) continue; // 其它类别(椅子/桌子…)与标定无关
        const int idx = cellIndex(d.box);
        if (idx < 0) continue;
        if (is_item) ++item_grid_[static_cast<std::size_t>(idx)];
        if (is_person) ++person_grid_[static_cast<std::size_t>(idx)];
    }
}

SeatProbe::Report SeatProbe::report() const {
    Report r;
    r.width = width_;
    r.height = height_;
    r.cols = cfg_.grid_cols;
    r.rows = cfg_.grid_rows;
    r.cell_w = cellSpan(width_, cfg_.grid_cols);
    r.cell_h = cellSpan(height_, cfg_.grid_rows);
    r.cells.reserve(item_grid_.size());
    for (int row = 0; row < cfg_.grid_rows; ++row) {
        for (int col = 0; col < cfg_.grid_cols; ++col) {
            const std::size_t i = static_cast<std::size_t>(row) * cfg_.grid_cols + col;
            Cell c;
            c.row = row;
            c.col = col;
            c.item_hits = item_grid_[i];
            c.person_hits = person_grid_[i];
            r.cells.push_back(c);
        }
    }

    // 建议 zone = 所有"达到 min_item_hits 的 item 热格"的最小外接矩形
    int min_col = cfg_.grid_cols, max_col = -1;
    int min_row = cfg_.grid_rows, max_row = -1;
    for (const Cell& c : r.cells) {
        if (c.item_hits < cfg_.min_item_hits) continue;
        min_col = std::min(min_col, c.col);
        max_col = std::max(max_col, c.col);
        min_row = std::min(min_row, c.row);
        max_row = std::max(max_row, c.row);
    }
    if (max_col >= min_col && max_row >= min_row && width_ > 0 && height_ > 0) {
        cv::Rect zone(min_col * r.cell_w, min_row * r.cell_h,
                      (max_col - min_col + 1) * r.cell_w,
                      (max_row - min_row + 1) * r.cell_h);
        // 裁剪回画面内(向上取整的格宽可能让最右/下格轻微越界)
        zone &= cv::Rect(0, 0, width_, height_);
        if (zone.width > 0 && zone.height > 0) {
            r.suggested_zone = zone;
            r.suggestion_valid = true;
        }
    }

    // 标签分布: 次数降序, 同次数按名字升序(保证输出可复现), 最多 12 条
    std::vector<std::pair<std::string, std::int64_t>> all(label_counts_.begin(),
                                                          label_counts_.end());
    std::sort(all.begin(), all.end(),
              [](const std::pair<std::string, std::int64_t>& a,
                 const std::pair<std::string, std::int64_t>& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    const std::size_t kTopLabels = 12;
    if (all.size() > kTopLabels) all.resize(kTopLabels);
    r.label_top = std::move(all);

    return r;
}

std::string SeatProbe::Report::describe() const {
    std::ostringstream os;
    os << "[seat-probe] 画面 " << width << "x" << height
       << ", 网格 " << cols << "x" << rows
       << " (每格 " << cell_w << "x" << cell_h << ")\n";
    os << "[seat-probe] 热力 (i=物品命中 p=人命中):\n";
    for (int row = 0; row < rows; ++row) {
        os << "  ";
        for (int col = 0; col < cols; ++col) {
            const Cell& c = cells[static_cast<std::size_t>(row) * cols + col];
            os << "r" << row << "c" << col << "(i" << c.item_hits << ",p" << c.person_hits
               << ")";
            if (col + 1 < cols) os << " ";
        }
        os << "\n";
    }
    if (!label_top.empty()) {
        os << "[seat-probe] 检出标签(次数降序): ";
        for (std::size_t i = 0; i < label_top.size(); ++i) {
            if (i) os << ", ";
            os << label_top[i].first << "=" << label_top[i].second;
        }
        os << "\n";
    }
    if (suggestion_valid) {
        os << "[seat-probe] 建议座位区 rect: [" << suggested_zone.x << ", "
           << suggested_zone.y << ", " << suggested_zone.width << ", "
           << suggested_zone.height << "]\n";
    } else {
        os << "[seat-probe] 物品落区命中不足, 无法给出建议(调小 min_item_hits 或换更长视频)\n";
    }
    return os.str();
}

std::string SeatProbe::Report::toJson() const {
    std::ostringstream os;
    os << "{\"width\":" << width << ",\"height\":" << height
       << ",\"cols\":" << cols << ",\"rows\":" << rows
       << ",\"cell_w\":" << cell_w << ",\"cell_h\":" << cell_h;

    os << ",\"cells\":[";
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const Cell& c = cells[i];
        if (i) os << ",";
        os << "{\"row\":" << c.row << ",\"col\":" << c.col
           << ",\"item_hits\":" << c.item_hits
           << ",\"person_hits\":" << c.person_hits << "}";
    }
    os << "]";

    os << ",\"suggestion_valid\":" << (suggestion_valid ? "true" : "false");
    if (suggestion_valid) {
        os << ",\"suggested_zone\":{\"x\":" << suggested_zone.x
           << ",\"y\":" << suggested_zone.y
           << ",\"width\":" << suggested_zone.width
           << ",\"height\":" << suggested_zone.height << "}";
    } else {
        os << ",\"suggested_zone\":null";
    }

    os << ",\"label_top\":[";
    for (std::size_t i = 0; i < label_top.size(); ++i) {
        if (i) os << ",";
        os << "{\"label\":\"" << jsonEscape(label_top[i].first)
           << "\",\"count\":" << label_top[i].second << "}";
    }
    os << "]";

    os << "}";
    return os.str();
}

} // namespace calibration
