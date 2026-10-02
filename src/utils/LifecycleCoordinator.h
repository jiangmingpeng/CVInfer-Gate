#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <signal.h>
#include <thread>
#include <vector>

// LifecycleCoordinator (T6: 生命周期协调器)
// 目标: 让"主线程 / gRPC 线程 / 视频流水线线程"通过 condition_variable
// 协作启动与优雅关闭。原 main.cpp 无生命周期管理, 视频跑完才启动
// gRPC, 也无法优雅退出。
//
// 主线程: waitUntilShutdown() 阻塞等待; 收到关闭请求后 waitForChildren()
// 子线程: registerChild()/unregisterChild() 登记; 用 isShutdownRequested()
// 轮询/判定是否该退出
class LifecycleCoordinator {
public:
    LifecycleCoordinator() = default;
    ~LifecycleCoordinator() = default;
    LifecycleCoordinator(const LifecycleCoordinator&) = delete;
    LifecycleCoordinator& operator=(const LifecycleCoordinator&) = delete;

    // 登记 / 注销一个需要在关闭时纳入管理的子系统
    void registerChild();
    void unregisterChild();

    // 请求关闭(幂等); 信号线程 / 任意线程均可调用
    void requestShutdown();
    bool isShutdownRequested() const;

    // 主线程阻塞, 直到收到关闭请求
    void waitUntilShutdown();

    // 等待所有登记的子任务退出; 返回是否在超时前全部退出
    bool waitForChildren(std::chrono::milliseconds timeout);

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_; // 主线程等待关闭请求
    std::condition_variable child_cv_; // 等待子任务退出
    bool shutdown_ = false;
    int running_children_ = 0;
};

// SignalWatcher (T6/T34: 异步信号安全监听)
// 捕获 SIGINT/SIGTERM, 收到即回调(回调在**普通线程**中执行, 故可以在里面
// 打日志/加锁/通知条件变量, 不受“异步信号安全”限制)。
//
// 实现从 "sigwait + 线程掩码继承" 改为 "sigaction + self-pipe":
// ── 旧方案的致命缺陷 ──
// 旧实现靠 "在 main 里先 pthread_sigmask 屏蔽 -> 之后创建的线程都继承掩码,
// 于是只有 sigwait 线程能收到信号"。这要求**进程里不存在任何未继承掩码的
// 线程**, 而这不受我们控制: 链接进来的库会在静态初始化阶段(main() 之前)
// 自己开线程, 那时掩码必为空(实测有 4 个这样的线程)。一旦内核把 SIGTERM
// 投给这类线程, 默认动作就是**直接杀死进程**, sigwait 永远等不到 -> 关闭
// 流程整段跳过(实测: Ctrl+C / kill -TERM 后一条关闭日志都没有)。
// ── 新方案 ──
// sigaction 注册的是**进程级**处理动作: 无论内核把信号投给哪个线程都会进
// handler, 不再赌任何库的线程掩码。handler 只做 async-signal-safe 的
// write() 到 self-pipe, 监听线程阻塞 read() 该管道后再回调。
class SignalWatcher {
public:
    using Callback = std::function<void(int)>;

    explicit SignalWatcher(Callback on_signal);
    ~SignalWatcher();

    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    // signals 默认 {SIGINT, SIGTERM}
    bool start(const std::vector<int>& signals = {SIGINT, SIGTERM});
    void stop();

private:
    void loop();

    Callback callback_;
    std::thread thread_;
    std::vector<int> signals_;
    std::atomic<bool> running_{false};
    int pipe_fds_[2] = {-1, -1}; // self-pipe: [0]=读端(监听线程) [1]=写端(信号处理器)
};
