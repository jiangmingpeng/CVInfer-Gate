#include "occupancy/SeatOccupancyAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <opencv2/imgproc.hpp> // pointPolygonTest / intersectConvexConvex

namespace occupancy {

namespace {

// 底边中点: "物体落在画面的哪个位置"的常用判据。
// 比框中心更贴近"放置"语义 —— 一个斜靠的书包, 框中心可能在半空, 底边中点才在桌面上。
cv::Point2f bottomCenter(const cv::Rect& r) {
    return cv::Point2f(static_cast<float>(r.x) + static_cast<float>(r.width) * 0.5f,
                       static_cast<float>(r.y + r.height));
}

cv::Point2f centerOf(const cv::Rect& r) {
    return cv::Point2f(static_cast<float>(r.x) + static_cast<float>(r.width) * 0.5f,
                       static_cast<float>(r.y) + static_cast<float>(r.height) * 0.5f);
}

float rectArea(const cv::Rect& r) {
    if (r.width <= 0 || r.height <= 0) return 0.0f;
    return static_cast<float>(r.width) * static_cast<float>(r.height);
}

bool insideRect(const cv::Point2f& p, const cv::Rect& r) {
    return p.x >= static_cast<float>(r.x) && p.x <= static_cast<float>(r.x + r.width) &&
           p.y >= static_cast<float>(r.y) && p.y <= static_cast<float>(r.y + r.height);
}

// inner 有多少比例的面积落在 outer 里 [0,1]。
// 为什么这里不能用 IoU: 判"物品在人手上/身上"是**包含关系**(书框几乎完全在人框内),
// 而真人框远大于一本书 => IoU = 书面积/人面积 天然很小(~0.05), 会漏判。
// 包含度则与"人框多大"无关, 这正是包含关系该用的度量。
float rectContainment(const cv::Rect& inner, const cv::Rect& outer) {
    const float a = rectArea(inner);
    if (a <= 0.0f) return 0.0f;
    const cv::Rect r = inner & outer;
    if (r.width <= 0 || r.height <= 0) return 0.0f;
    return static_cast<float>(r.area()) / a;
}

std::vector<cv::Point2f> toFloatPoints(const std::vector<cv::Point>& poly) {
    std::vector<cv::Point2f> out;
    out.reserve(poly.size());
    for (const auto& p : poly) out.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
    return out;
}

// C5: 有界投票窗口
void pushVote(std::deque<bool>& q, bool v, int n) {
    q.push_back(v);
    while (static_cast<int>(q.size()) > n) q.pop_front();
}

int countTrue(const std::deque<bool>& q) {
    int c = 0;
    for (bool b : q) {
        if (b) ++c;
    }
    return c;
}

std::string joinLabels(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += v[i];
    }
    return s;
}

} // namespace

const std::vector<std::string>& defaultItemLabels() {
    // COCO(YOLOv8n 直接可给) 与自训练模型都常见的"能被人放在座位上的东西"。
    // 注意: cup/bottle 容易误报(常在手边/窗台), 若噪声大可在配置里只留 bag/laptop/book。
    static const std::vector<std::string> kLabels = {
        "bag", "backpack", "handbag", "laptop", "book", "cup", "bottle", "suitcase"};
    return kLabels;
}

const char* seatStateToString(SeatState s) {
    switch (s) {
        case SeatState::Pending:  return "pending";
        case SeatState::Occupied: return "occupied";
        case SeatState::Idle:
        default:                  return "idle";
    }
}

cv::Rect SeatZone::boundingRect() const {
    if (polygon.empty()) return cv::Rect();
    int x1 = polygon.front().x, y1 = polygon.front().y;
    int x2 = x1, y2 = y1;
    for (const auto& p : polygon) {
        x1 = std::min(x1, p.x);
        y1 = std::min(y1, p.y);
        x2 = std::max(x2, p.x);
        y2 = std::max(y2, p.y);
    }
    return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

float SeatEvidence::confidence() const {
    if (vote_total <= 0) return 0.0f;
    if (vote_total == 1) return 1.0f; // 未启用投票 => 单帧结论, 视为"确定"
    return static_cast<float>(vote_hit) / static_cast<float>(vote_total);
}

std::string SeatEvidence::describe() const {
    std::ostringstream os;
    os << "座位 " << (seat.empty() ? std::string("(未命名)") : seat) << ": ";
    if (item_count == 0) {
        os << "未检出物品";
        if (state != SeatState::Idle) os << "(状态=" << seatStateToString(state) << ")";
        return os.str();
    }
    os << "检出物品 [" << joinLabels(item_labels) << "]";
    os << ", 已连续 " << (dwell_ms / 1000) << " 秒「有物品且无人使用」";
    if (last_person_age_ms >= 0) {
        os << ", 最近一次检测到人在座位上是 " << (last_person_age_ms / 1000) << " 秒前";
    } else {
        os << ", 全程未检测到人";
    }
    if (vote_total > 1) {
        os << "; 近 " << vote_total << " 帧中 " << vote_hit << " 帧满足该条件";
    }
    return os.str();
}

float SeatOccupancyAnalyzer::containment(const cv::Rect& box,
                                         const std::vector<cv::Point>& polygon) {
    const float a = rectArea(box);
    if (a <= 0.0f || polygon.size() < 3) return 0.0f;

    const std::vector<cv::Point2f> rect_pts{
        cv::Point2f(static_cast<float>(box.x), static_cast<float>(box.y)),
        cv::Point2f(static_cast<float>(box.x + box.width), static_cast<float>(box.y)),
        cv::Point2f(static_cast<float>(box.x + box.width), static_cast<float>(box.y + box.height)),
        cv::Point2f(static_cast<float>(box.x), static_cast<float>(box.y + box.height))};
    const std::vector<cv::Point2f> poly_pts = toFloatPoints(polygon);

    std::vector<cv::Point2f> inter;
    double inter_area = 0.0;
    try {
        // 求两个凸多边形交集面积(内部会各自求凸包, 故对顶点顺序不敏感)
        inter_area = cv::intersectConvexConvex(rect_pts, poly_pts, inter, true);
    } catch (const cv::Exception&) {
        return 0.0f; // 退化/自交多边形: 宁可不判占座, 也不误报
    }
    if (!(inter_area > 0.0)) return 0.0f;
    const double ratio = inter_area / static_cast<double>(a);
    return static_cast<float>(std::min(std::max(ratio, 0.0), 1.0));
}

bool SeatOccupancyAnalyzer::pointInPolygon(const cv::Point2f& p,
                                           const std::vector<cv::Point>& polygon) {
    if (polygon.size() < 3) return false;
    // measureDist=false; >=0 表示在区域内**或边上**(边界算"在", 与 zone 语义一致)
    return cv::pointPolygonTest(toFloatPoints(polygon), p, false) >= 0.0;
}

float SeatOccupancyAnalyzer::iou(const cv::Rect& a, const cv::Rect& b) {
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0) return 0.0f;
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    const int iw = x2 - x1;
    const int ih = y2 - y1;
    if (iw <= 0 || ih <= 0) return 0.0f;
    const float inter = static_cast<float>(iw) * static_cast<float>(ih);
    const float uni = rectArea(a) + rectArea(b) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

bool SeatOccupancyAnalyzer::isItemLabel(const std::string& label,
                                        const std::vector<std::string>& item_labels) {
    return std::find(item_labels.begin(), item_labels.end(), label) != item_labels.end();
}

SeatOccupancyAnalyzer::SeatOccupancyAnalyzer(Config cfg, std::vector<SeatZone> seats)
    : cfg_(std::move(cfg)), zones_(std::move(seats)) {
    // 防御: 配置层已校验, 这里兜一层(本组件声明可独立复用)
    if (cfg_.vote_n < 1) cfg_.vote_n = 1;
    if (cfg_.vote_m < 1) cfg_.vote_m = 1;
    if (cfg_.vote_m > cfg_.vote_n) cfg_.vote_m = cfg_.vote_n;
    if (cfg_.t_occupied_ms < 0) cfg_.t_occupied_ms = 0;
    if (cfg_.t_grace_ms < 0) cfg_.t_grace_ms = 0;
    if (cfg_.item_labels.empty()) cfg_.item_labels = defaultItemLabels();
    states_.resize(zones_.size());
}

bool SeatOccupancyAnalyzer::itemOnSeat(const DetectionResult& d, const SeatZone& z,
                                       const std::vector<DetectionResult>& dets) const {
    // C1: 物品"落在座位上"(底边中点在区内, 或与座位区的包含度够高)
    const bool on = pointInPolygon(bottomCenter(d.box), z.polygon) ||
                    containment(d.box, z.polygon) >= cfg_.item_seat_overlap;
    if (!on) return false;

    // C1b: 被同一帧里的人**拿着/背着** => 随身物品, 不是"放在座位上"。
    // 没有这条, "有人拎着包从桌边走过"会被记成"包放在桌上"。
    // 注意用**包含度**而非 IoU(理由见 rectContainment): 判的是"物品落不落在人身上",
    // 与"人框多大"无关。
    for (const auto& p : dets) {
        if (!usablePerson(p)) continue;
        if (rectContainment(d.box, p.box) < cfg_.item_person_overlap) continue;
        if (insideRect(centerOf(d.box), p.box)) return false;
    }
    return true;
}

// "可信的人框": (a) 确实是人 且 (b) 高度够大 —— 两处共用同一把尺:
// 过小的框(远景/误检)既不算"在用座位", 也不算"拿着物品", 避免
// "一个被过滤掉的人框却能透过 C1b 把物品排除掉"的自相矛盾。
// 注意 (a) 必须判: 否则物品会与**自己**做包含度比较(=1.0) 而被判成"手持"。
bool SeatOccupancyAnalyzer::usablePerson(const DetectionResult& d) const {
    if (d.label != cfg_.person_label) return false;
    if (cfg_.min_person_height_px > 0 && d.box.height < cfg_.min_person_height_px) return false;
    return true;
}

bool SeatOccupancyAnalyzer::personUsingSeat(const DetectionResult& d, const SeatZone& z) const {
    // 远景色小框(误检/隔壁机位的人)先滤掉
    if (!usablePerson(d)) return false;
    // C2: 人"在这个座位位置"(底边中点 = 脚/臀所在处)
    return pointInPolygon(bottomCenter(d.box), z.polygon) ||
           containment(d.box, z.polygon) >= cfg_.person_seat_iou;
}

SeatEvidence SeatOccupancyAnalyzer::buildEvidence(std::size_t idx, const SeatStateData& st,
                                                  int item_count,
                                                  std::vector<std::string> item_labels,
                                                  std::int64_t now_ms) const {
    SeatEvidence ev;
    ev.seat = zones_[idx].name;
    ev.box = zones_[idx].boundingRect();
    ev.state = st.state;
    ev.item_count = item_count;
    ev.item_labels = std::move(item_labels);
    ev.dwell_ms = st.accum_ms;
    ev.last_person_age_ms = (st.last_person_ms >= 0) ? (now_ms - st.last_person_ms) : -1;
    ev.vote_hit = countTrue(st.occ_votes);
    ev.vote_total = static_cast<int>(st.occ_votes.size());
    return ev;
}

std::vector<OccupancyEvent> SeatOccupancyAnalyzer::update(
    const std::vector<DetectionResult>& dets, std::int64_t now_ms) {
    std::vector<OccupancyEvent> events;
    if (!enabled()) return events;
    ++frames_;

    evidence_.clear();
    evidence_.reserve(zones_.size());

    for (std::size_t i = 0; i < zones_.size(); ++i) {
        SeatStateData& st = states_[i];
        const SeatZone& z = zones_[i];

        // ---- 单帧几何判定 ----
        int item_count = 0;
        std::vector<std::string> item_labels;
        for (const auto& d : dets) {
            if (!isItemLabel(d.label, cfg_.item_labels)) continue;
            if (!itemOnSeat(d, z, dets)) continue;
            ++item_count;
            if (item_labels.size() < 8) item_labels.push_back(d.label); // 证据里最多列 8 个
        }
        bool person_here = false;
        for (const auto& d : dets) {
            if (d.label != cfg_.person_label) continue;
            if (personUsingSeat(d, z)) {
                person_here = true;
                break;
            }
        }

        // ---- C5: M-of-N 投票(去抖动) ----
        // 三个窗口各司其职: "物品在" / "人不在" / "两者同时成立" —— 前两者用于区分
        // "物品被拿走"与"人回来了"(两者复位语义不同), 后者才是状态推进与证据命中的依据。
        const bool occ_now = (item_count > 0) && !person_here;
        pushVote(st.item_votes, item_count > 0, cfg_.vote_n);
        pushVote(st.noperson_votes, !person_here, cfg_.vote_n);
        pushVote(st.occ_votes, occ_now, cfg_.vote_n);
        const int seen = static_cast<int>(st.occ_votes.size());
        // 窗口未满时按"已见帧数"取阈值 => 启用后的头几帧也能正常判定(否则要等满 N 帧)
        const int need = std::min(cfg_.vote_m, seen);
        const bool v_item = countTrue(st.item_votes) >= need;
        const bool v_no_person = countTrue(st.noperson_votes) >= need;
        const bool v_occ = countTrue(st.occ_votes) >= need;

        // ---- C3/C4: 时序状态机 ----
        const std::int64_t delta =
            (st.last_ms >= 0) ? std::max<std::int64_t>(0, now_ms - st.last_ms) : 0;
        st.last_ms = now_ms;

        const SeatState prev = st.state;
        if (!v_item) {
            // 物品稳定地不在了 => 复位(被收走 / 被拿走)
            st.accum_ms = 0;
            st.person_since_ms = -1;
            st.state = SeatState::Idle;
        } else if (!v_no_person) {
            // 人稳定在场 => C4: 短暂出现只**暂停**累计, 够久才复位
            if (st.person_since_ms < 0) st.person_since_ms = now_ms;
            st.last_person_ms = now_ms;
            if (now_ms - st.person_since_ms >= cfg_.t_grace_ms) {
                st.accum_ms = 0;
                st.state = SeatState::Idle;
            }
            // else: 保持状态并暂停累计 —— 单帧误检不会把几分钟的计时打回原点
        } else if (v_occ) {
            // 物品在 且 人稳定不在 => 累计(C3)
            st.person_since_ms = -1;
            st.accum_ms += delta;
            st.state = (st.accum_ms >= cfg_.t_occupied_ms) ? SeatState::Occupied
                                                          : SeatState::Pending;
        } else {
            // 两个条件各自成立却"从未同时"成立(检出抖动/交替): 条件不稳定 =>
            // 同样只**暂停**累计, 不清零(与 C4 同一哲学: 宁可多等, 不可把已有证据清零)
            st.person_since_ms = -1;
        }

        SeatEvidence ev = buildEvidence(i, st, item_count, std::move(item_labels), now_ms);

        // ---- C6: 上升沿触发(每个座位每次占用只出一个事件) ----
        if (prev != SeatState::Occupied && st.state == SeatState::Occupied) {
            ++events_;
            OccupancyEvent e;
            e.seat = z.name;
            e.box = ev.box;
            e.evidence = ev;
            events.push_back(std::move(e));
        }
        evidence_.push_back(std::move(ev));
    }
    return events;
}

SeatOccupancyAnalyzer::Stats SeatOccupancyAnalyzer::stats() const {
    Stats s;
    s.frames = frames_;
    s.events = events_;
    s.seats = zones_.size();
    return s;
}

} // namespace occupancy
