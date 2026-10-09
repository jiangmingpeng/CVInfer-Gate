#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "inference/DetectionResult.h"

// SeatOccupancyAnalyzer (占座判定: 静态座位 zone + 几何关系 + 时序状态机)
//
// ---------------------------------------------------------------------------
// 为什么需要这一层(而不是"再加一个模型")
// ---------------------------------------------------------------------------
// 检测器只能给出"画面里有一个 book / 一个 bag"。而"占座"这个结论 = 「**某个座位**
// 上 **物品在、人不在了**」, 它由两个**任何单帧检测器都给不出**的东西构成:
//   ① 空间关系: 这个物品属于**哪个座位**?   (检测器没有"座位"这个概念 ——
//      library_detector 只有 person/bag/laptop/book 四类)
//   ② 时间演化: 这种状态**已经持续多久**了?  (单帧推理天然无记忆)
//
// 把这两件事塞给 VLM 也不行: 它拿到的是一张(可能只有 320x387 的)图, 既看不见
// 座位全貌, 也无法回答"持续了多久"。所以这里用一个**可解释、可单测、可调参**的
// 规则状态机来判定, 让 VLM 退回到它真正擅长位置 —— 对**已有证据**做复核/兜底。
//
// 这也解释了为什么"两个检测器级联"解决不了问题: 级联(CascadeEngine)是
// "同一目标、二分类细化", 而占座是"多目标空间关系 + 时序", 属于关系推理。
//
// ---------------------------------------------------------------------------
// 判定标准(每条都独立可测; 见 tests/unit/test_seat_occupancy.cpp)
// ---------------------------------------------------------------------------
// C1  物品归属座位 ItemOnSeat   : 物品框**底边中点**落在座位多边形内, 或与座位区的
//                                 面积包含度 >= item_seat_overlap
//                                 (底边中点 = "东西放在哪里"的常用判据, 比框中心更稳)
// C1b 物品不在人手上             : 物品面积落在某 person 框内的比例 >= item_person_overlap
//                                 且物品中心也在该人框内 => 视为手持/随身, **不算**放在座位上。
//                                 用包含度而非 IoU: 真人框远大于一本书, IoU 天然很小
//                                 (书面积/人面积 ≈ 0.05) 会漏判; 包含度与"人框多大"无关
//                                 (若人框低于 min_person_height_px, 则不参与本判据)
// C2  人在使用座位 PersonUsingSeat: 人框**底边中点**落在座位内, 或包含度 >=
//                                 person_seat_iou; 且人框高 >= min_person_height_px
//                                 (滤掉远景小框/误检)
// C3  持续时间 t_occupied_ms     : "物品在 ∧ 人不在"的**累计时长**够久 => 占座。
//                                 这才是"长期占用"的正确定义 —— 不是"物品存在久"
// C4  短暂离开容忍 t_grace_ms     : 人**短暂**出现(不足 t_grace)时**暂停**累计而不清零;
//                                 人**稳定**在场 >= t_grace 才复位。
//                                 => 单帧误检/有人路过不会把几分钟的计时打回原点
// C5  稳定性投票 vote_n/vote_m    : 最近 N 帧中至少 M 帧满足才算数(去抖动)
// C6  去重                       : 本类**上升沿触发**(每个座位每次占用只出一个事件);
//                                 兜底去重仍交给 main 的 AlertGate(静态框 => 几何去重可靠)
// C7  证据 evidence              : 输出物品列表 / 已持续时长 / 最近见到人的时刻 / 投票
//                                 情况, 既写日志也拼进送审提示词 —— 让 VLM 有据可依
// C1/C2 前置 排他归属             : 同一个目标(物品/人)每帧**最多只归属一个座位**。
//                                 座位相邻/重叠时一个目标会同时命中多座; 命中多座就按
//                                 "证据最强"(底边中点在区内优先, 再看包含度)只算一个。
//                                 否则一个物品会把多座**同时**判成被占(乱覆盖), 一个人
//                                 会把多座计时**同时**压住(漏报)。平局取先配置的座位(确定性)。
//
// ---------------------------------------------------------------------------
// 边界(诚实说明)
// ---------------------------------------------------------------------------
// * **座位靠配置画, 不靠模型猜**: 监控机位固定 => 座位位置是常量。让模型每帧去
//   检测一个常量既不可靠(俯视图上 COCO 的 chair/table 召回很差)又浪费算力。
//   "把确定性的事交给配置, 把不确定的事交给模型"。
// * 多边形按**凸多边形**处理(用 cv::intersectConvexConvex 求面积); 座位 zone 通常
//   就是矩形/四边形, 够用。自交/退化多边形会退化为"包含度=0"(宁可不判占座, 不误报)。
// * "人在不在"只看**单帧几何**+投票, 不做人的身份跟踪。同一座位前后换了不同的人
//   对本规则无影响(它判的是"座位有没有人用", 不是"谁在用")。
// * 线程安全: 与 tracking::TargetTracker 一致 —— **非线程安全**, 由调用方在单个
//   sink 线程里顺序调用(见 main.cpp: 重排缓冲保证 frame_seq 单调递增)。
namespace occupancy {

// 座位区域: 一个凸多边形(矩形也用它表达)。
// 用多边形而非 rect 是为了贴合透视 —— 斜视机位下"桌面"是梯形, 用矩形会把邻座框进来。
struct SeatZone {
    std::string name;
    std::vector<cv::Point> polygon; // >= 3 点

    // 外接矩形(用于告警去重 / 送审裁剪 / 日志)
    cv::Rect boundingRect() const;
};

// 内置默认"物品"标签: 通用于 COCO(YOLOv8n) 与自训练模型。
// 刻意**不含** person/chair/table 这类"人/环境"类别 —— 它们是判据, 不是"物品"。
const std::vector<std::string>& defaultItemLabels();

struct Config {
    bool enabled = false;
    std::vector<std::string> item_labels; // 空 => 用 defaultItemLabels()
    std::string person_label = "person";
    float item_seat_overlap = 0.5f;   // C1  包含度阈值 [0,1]
    float item_person_overlap = 0.5f; // C1b 物品面积落在人框内的比例 >= 该值 => 视为手持 [0,1]
    // C2 人的包含度阈值 [0,1]。取 0.30(而非旧的 0.15): 15% 太松 —— 俯视机位下邻座/
    // 路过的人框只要与座位区重叠一点点就会命中 C2, 于是本座位的"物品在 ∧ 人不在"计时
    // 被反复暂停/清零 => 真占座反而漏报(假阴性)。0.30 要求"人确实落在座位上"才命中。
    float person_seat_iou = 0.30f;
    int min_person_height_px = 0;     // C2  人框最小高度(0 = 不启用该过滤)
    std::int64_t t_occupied_ms = 300000; // C3 判占座的持续时间(ms)
    std::int64_t t_grace_ms = 90000;     // C4 短暂离开容忍(ms)
    int vote_n = 10;                  // C5 投票窗口
    int vote_m = 7;                   // C5 窗口内至少命中帧数
};

enum class SeatState { Idle, Pending, Occupied };
const char* seatStateToString(SeatState s);

// C7: 一个座位的当前证据(可直接写日志 / 拼进送审提示词)
struct SeatEvidence {
    std::string seat;                     // 座位名
    cv::Rect box;                         // 座位外接框
    SeatState state = SeatState::Idle;
    int item_count = 0;                   // 座位上检出的物品数
    std::vector<std::string> item_labels; // 物品标签(最多列 8 个)
    std::int64_t dwell_ms = 0;            // C3: "物品在 ∧ 人不在" 已累计多久
    std::int64_t last_person_age_ms = -1; // 距最近一次"人在座位"多久(-1 = 全程未见到人)
    int vote_hit = 0;                     // C5: 窗口内命中帧数
    int vote_total = 0;                   // C5: 窗口内实际帧数(窗口未满时为已见帧数)

    bool occupied() const { return state == SeatState::Occupied; }
    // 规则判定的"置信度"= 投票命中率 [0,1](诚实口径: 它衡量的是**证据一致性**,
    // 不是模型概率)。不做投票时(vote_n=1)恒为 1.0。
    float confidence() const;
    // 人可读证据(日志 / 送审提示词共用同一份文案, 避免"日志与送审说不一致")
    std::string describe() const;
};

// C6: 上升沿事件(某座位"刚刚"被判定为占座)
struct OccupancyEvent {
    std::string seat;
    cv::Rect box;
    SeatEvidence evidence;
};

class SeatOccupancyAnalyzer {
public:
    SeatOccupancyAnalyzer(Config cfg, std::vector<SeatZone> seats);

    // 每帧调用一次(dets = 本帧最终检测结果; now_ms = **单调**时钟毫秒)
    // 返回: 本帧"刚刚进入占座"的座位(上升沿; 每个座位每次占用只出一次)
    std::vector<OccupancyEvent> update(const std::vector<DetectionResult>& dets,
                                       std::int64_t now_ms);

    // 最近一次 update 的全部座位证据(供日志/运维观察)
    const std::vector<SeatEvidence>& snapshot() const { return evidence_; }

    const std::vector<SeatZone>& seats() const { return zones_; }
    bool enabled() const { return cfg_.enabled && !zones_.empty(); }
    const Config& config() const { return cfg_; }

    struct Stats {
        std::uint64_t frames = 0; // 已处理帧数
        std::uint64_t events = 0; // 累计占座事件数(上升沿)
        std::size_t seats = 0;    // 座位数
    };
    Stats stats() const;

    // ---- 纯几何判据(public static: 单测可直接验证几何, 不必构造状态机) ----
    // 矩形与多边形的交集面积 / 矩形面积, 结果裁剪到 [0,1]。退化多边形返回 0。
    static float containment(const cv::Rect& box, const std::vector<cv::Point>& polygon);
    // 点是否在多边形内(边界算"在内")
    static bool pointInPolygon(const cv::Point2f& p, const std::vector<cv::Point>& polygon);
    static float iou(const cv::Rect& a, const cv::Rect& b);
    static bool isItemLabel(const std::string& label, const std::vector<std::string>& item_labels);

private:
    struct SeatStateData {
        std::deque<bool> item_votes;     // 最近 N 帧: 座位上有物品?(用于"物品被拿走"的复位判定)
        std::deque<bool> noperson_votes; // 最近 N 帧: 座位上没人?(用于"人回来了"的暂停/复位判定)
        std::deque<bool> occ_votes;      // 最近 N 帧: C1∧¬C2 **同时**成立?
                                         //   与上两个窗口分开存: "物品稳定在"与"人稳定不在"
                                         //   分别成立, 并不等于"两者同时成立"。
                                         //   它既是状态推进的依据, 也是证据命中率的来源。
        std::int64_t accum_ms = 0;        // C3: 累计时长(C4 会让它"暂停")
        std::int64_t last_ms = -1;        // 上一帧时间(算 delta)
        std::int64_t last_person_ms = -1; // 最近一次"人在座位"的绝对时刻
        std::int64_t person_since_ms = -1;// "人稳定在场"的起点(C4)
        SeatState state = SeatState::Idle;
    };

    bool itemOnSeat(const DetectionResult& d, const SeatZone& z,
                    const std::vector<DetectionResult>& dets) const;
    bool personUsingSeat(const DetectionResult& d, const SeatZone& z) const;
    // 可信的人框(高度过滤): "在用座位"与"拿着物品"两处共用同一把尺
    bool usablePerson(const DetectionResult& d) const;

    // ---- P0: 目标 -> 座位的**排他归属**(见文件头 "C1/C2 前置 排他归属") ----
    // 单座位的归属打分: 底边中点在区内(强, +2.0) + 包含度(弱, 0..1)
    float seatScore(const DetectionResult& d, const SeatZone& z) const;
    // 该物品应归属的座位(命中多座时取分最高者; 不属于任何座位 => -1)
    int bestSeatForItem(const DetectionResult& d,
                        const std::vector<DetectionResult>& dets) const;
    // 该人应归属的座位(同上; 不属于任何座位 => -1)
    int bestSeatForPerson(const DetectionResult& d) const;
    SeatEvidence buildEvidence(std::size_t idx, const SeatStateData& st, int item_count,
                               std::vector<std::string> item_labels, std::int64_t now_ms) const;

    Config cfg_;
    std::vector<SeatZone> zones_;
    std::vector<SeatStateData> states_; // 与 zones_ 同长同序
    std::vector<SeatEvidence> evidence_;
    std::uint64_t frames_ = 0;
    std::uint64_t events_ = 0;
};

} // namespace occupancy
