#include "utils/LifecycleCoordinator.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

// ------------------------- LifecycleCoordinator -------------------------

void LifecycleCoordinator::registerChild() {
    std::unique_lock<std::mutex> lock(mtx_);
    ++running_children_;
}

void LifecycleCoordinator::unregisterChild() {
    {
        std::unique_lock<std::mutex> lock(mtx_);
        if (running_children_ > 0) --running_children_;
    }
    child_cv_.notify_all();
}

void LifecycleCoordinator::requestShutdown() {
    {
        std::unique_lock<std::mutex> lock(mtx_);
        if (shutdown_) return;   // 幂等
        shutdown_ = true;
    }
    cv_.notify_all();
}

bool LifecycleCoordinator::isShutdownRequested() const {
    std::unique_lock<std::mutex> lock(mtx_);
    return shutdown_;
}

void LifecycleCoordinator::waitUntilShutdown() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return shutdown_; });
}

bool LifecycleCoordinator::waitForChildren(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mtx_);
    return child_cv_.wait_for(lock, timeout, [this] { return running_children_ == 0; });
}

// --------------------------- SignalWatcher ---------------------------

namespace {
// [T34] 信号处理器只能调用 async-signal-safe 的函数, 且只能碰 sig_atomic_t 这类
//   保证读写原子性的对象。这里只做一件事: 把信号号写进 self-pipe 的写端。
//   绝不能在 handler 里打日志/加锁/分配内存。
//   写端必须是 O_NONBLOCK —— 管道写满时 write 若阻塞, 信号处理就被卡死。
volatile sig_atomic_t g_pipe_wr = -1;

void handleSignal(int sig) {
    const int fd = static_cast<int>(g_pipe_wr);
    if (fd < 0) return;
    const char c = static_cast<char>(sig);
    const ssize_t n = ::write(fd, &c, 1);   // async-signal-safe
    (void)n;                                // 管道满(EAGAIN)则丢弃: 关闭流程只做一次
}
}  // namespace

SignalWatcher::SignalWatcher(Callback on_signal) : callback_(std::move(on_signal)) {}

SignalWatcher::~SignalWatcher() {
    stop();
}

bool SignalWatcher::start(const std::vector<int>& signals) {
    if (running_) return true;

    if (::pipe(pipe_fds_) != 0) {
        return false;
    }
    // 读端留给监听线程(阻塞读); 写端必须非阻塞(见 handleSignal 注释)。
    const int flags = ::fcntl(pipe_fds_[1], F_GETFL, 0);
    if (flags >= 0) ::fcntl(pipe_fds_[1], F_SETFL, flags | O_NONBLOCK);
    ::fcntl(pipe_fds_[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(pipe_fds_[1], F_SETFD, FD_CLOEXEC);

    // 先让 handler 拿得到写端, 再安装处理动作(顺序不能反)。
    // 注意: 这里**不再** pthread_sigmask —— sigaction 是进程级的, 不论内核把信号
    //   投给哪个线程都会进 handleSignal(旧 sigwait 方案依赖掩码继承, 已废弃:T34)。
    g_pipe_wr = pipe_fds_[1];

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // 不设 SA_RESTART: read() 被打断时返回 EINTR, loop() 会重试
    signals_ = signals;
    for (int s : signals_) {
        if (::sigaction(s, &sa, nullptr) != 0) {
            g_pipe_wr = -1;
            signals_.clear();
            ::close(pipe_fds_[0]);
            ::close(pipe_fds_[1]);
            pipe_fds_[0] = pipe_fds_[1] = -1;
            return false;
        }
    }

    running_ = true;
    thread_ = std::thread([this] { loop(); });
    return true;
}

void SignalWatcher::loop() {
    while (running_) {
        char c = 0;
        const ssize_t n = ::read(pipe_fds_[0], &c, 1);
        if (n == 1) {
            if (!running_) break;   // stop() 用一字节唤醒本线程
            if (callback_) callback_(static_cast<int>(static_cast<unsigned char>(c)));
        } else if (n == 0) {
            break;                  // 写端已关闭
        } else if (errno != EINTR) {
            break;                  // 真错误
        }
        // n < 0 且 EINTR: 重试
    }
}

void SignalWatcher::stop() {
    if (!running_) return;
    running_ = false;

    // 1) 先让 handler 失效并还原默认处理: 之后到来的信号回到系统默认行为,
    //    不会出现“写了字节但监听线程已退出”的空写。
    g_pipe_wr = -1;
    for (int s : signals_) {
        ::signal(s, SIG_DFL);
    }

    // 2) 写一字节唤醒阻塞在 read() 的监听线程
    if (pipe_fds_[1] >= 0) {
        const char c = 0;
        const ssize_t n = ::write(pipe_fds_[1], &c, 1);
        (void)n;
    }
    if (thread_.joinable()) thread_.join();

    // 3) 收尾
    if (pipe_fds_[0] >= 0) { ::close(pipe_fds_[0]); pipe_fds_[0] = -1; }
    if (pipe_fds_[1] >= 0) { ::close(pipe_fds_[1]); pipe_fds_[1] = -1; }
    signals_.clear();
}
