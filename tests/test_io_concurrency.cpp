// 边界测试：1000 个协程挂起等待 IO，进程 CPU 接近 0；数据到达后全部唤醒
#include "fd_manager.h"
#include "hook.h"
#include "iomanager.h"
#include "util.h"
#include <atomic>
#include <cstdio>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace sylar;

static const int N = 1000;

// 累计用户态+内核态 CPU 时间（秒）
static double cputime(const struct rusage& r) {
    return (double)r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6
         + (double)r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}

int main() {
    // 1000 个 socketpair = 2000 个 fd，抬高 fd 上限
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = 8192;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        perror("setrlimit");
        return 1;
    }

    int pairs[N][2];
    for (int i = 0; i < N; ++i) {
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]) != 0) {
            perror("socketpair");
            return 1;
        }
        // socketpair 不经 hook，需手动注册 FdCtx（置 O_NONBLOCK）
        FdMgr::GetInstance()->get(pairs[i][0], true);
        FdMgr::GetInstance()->get(pairs[i][1], true);
    }

    IOManager iom(1, true, "io_concurrency");
    std::atomic<int> done{0};

    // N 个读协程：全部挂起在各自 fd 上等待数据
    for (int i = 0; i < N; ++i) {
        iom.schedule([&, i]() {
            char buf[4];
            if (read(pairs[i][0], buf, sizeof(buf)) == 1) {
                done.fetch_add(1);
            }
        });
    }

    // 写协程：300ms 后唤醒全部读协程（usleep 被 hook 成定时器）
    iom.schedule([&]() {
        usleep(300 * 1000);
        for (int i = 0; i < N; ++i) {
            write(pairs[i][1], "x", 1);
        }
    });

    uint64_t t0 = GetCurrentMS();
    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    iom.stop();
    getrusage(RUSAGE_SELF, &ru1);
    double elapsed = (GetCurrentMS() - t0) / 1000.0;

    for (int i = 0; i < N; ++i) {
        close(pairs[i][0]);
        close(pairs[i][1]);
    }

    double cpu_pct = (cputime(ru1) - cputime(ru0)) / elapsed * 100.0;
    printf("elapsed=%.2fs  cpu_used=%.2f%% (of one core)  woken=%d/%d\n",
           elapsed, cpu_pct, done.load(), N);

    if (done.load() != N) {
        printf("FAIL: not all coroutines woken\n");
        return 1;
    }
    if (cpu_pct > 15.0) {
        printf("FAIL: CPU too high while suspended (busy-wait?)\n");
        return 1;
    }
    printf("PASS: 1000 suspended coroutines ~0%% CPU, all woken on data\n");
    return 0;
}