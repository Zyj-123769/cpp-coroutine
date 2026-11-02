// 多线程调度器 / 循环定时器 / 同步原语 的组合测试。
//
// 补上此前缺失的覆盖：
//   - 其它 IO 测试都是 IOManager(1, ...)，没有验证「多 worker 线程」下的事件分发
//     与跨线程唤醒（协程 A 在 worker1 挂起，worker2 写入后由 idle 唤起）；
//   - 没有 recurring=true 的循环定时器用例（只有一次性定时器）；
//   - Thread / Mutex 等同步原语没有独立测试。
#include "fd_manager.h"
#include "hook.h"
#include "iomanager.h"
#include "mutex.h"
#include "thread.h"
#include "timer.h"
#include "util.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

using namespace sylar;

static const int kWorkers = 4;   // 3 个 worker 线程 + 调用线程
static const int N = 64;         // 跨线程唤醒的协程数
static const int kTicks = 5;     // 循环定时器期望触发次数

int main() {
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = 4096;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        perror("setrlimit");
        return 1;
    }

    int fail = 0;

    // ---------- 1) 多线程 IOManager：跨线程挂起/唤醒 ----------
    {
        IOManager iom(kWorkers, true, "multi");

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

        std::atomic<int> woken{0};
        std::atomic<int> ticks{0};
        std::mutex tid_mu;
        std::set<long> tids;

        for (int i = 0; i < N; ++i) {
            iom.schedule([&, i]() {
                {
                    std::lock_guard<std::mutex> g(tid_mu);
                    tids.insert((long)::syscall(SYS_gettid));
                }
                char buf[4];
                if (read(pairs[i][0], buf, sizeof(buf)) == 1) {
                    woken.fetch_add(1);
                }
            });
        }

        // 写协程：200ms 后唤醒全部读协程（usleep 被 hook 成定时器）
        iom.schedule([&]() {
            usleep(200 * 1000);
            for (int i = 0; i < N; ++i) {
                write(pairs[i][1], "x", 1);
            }
        });

        // 循环定时器：每 20ms 一次，第 kTicks 次后自行 cancel。
        // listExpiredCb 在回调执行前已把该 timer 重新插入 m_timers，因此回调内 cancel
        // 能正常找到并移除；用 weak_ptr 取自身，避免 cb 与 timer 形成引用环。
        std::weak_ptr<Timer> wrec;
        Timer::ptr rec = iom.addTimer(20, [&]() {
            int t = ticks.fetch_add(1) + 1;
            if (t >= kTicks) {
                if (auto p = wrec.lock()) {
                    p->cancel();
                }
            }
        }, true);
        wrec = rec;

        // stop() 会等在跑的协程全部完成
        iom.stop();

        for (int i = 0; i < N; ++i) {
            close(pairs[i][0]);
            close(pairs[i][1]);
        }

        size_t ntid = 0;
        {
            std::lock_guard<std::mutex> g(tid_mu);
            ntid = tids.size();
        }

        printf("workers=%d  woken=%d/%d  recurring_ticks=%d  distinct_threads=%zu\n",
               kWorkers, woken.load(), N, ticks.load(), ntid);

        if (woken.load() != N) {
            printf("FAIL: only %d/%d coroutines woken (multi-thread event dispatch)\n",
                   woken.load(), N);
            fail = 1;
        }
        if (ticks.load() != kTicks) {
            printf("FAIL: recurring timer fired %d times, expect exactly %d "
                   "(self-cancel after N ticks)\n", ticks.load(), kTicks);
            fail = 1;
        }
        // 线程分布只作观测，不参与判定：任务入队后确实可能被某一个 worker 连续取走
        // （每次 read 立即 EAGAIN 挂起，取任务循环很快），据此判失败会引入 flaky。
        // 这个用例的价值在于「多线程下的事件分发/跨线程唤醒不崩、不丢唤醒」。
        if (ntid < 2) {
            printf("WARN: coroutines all ran on 1 thread this round (not a failure)\n");
        }
    }

    // ---------- 2) 同步原语：Mutex 互斥（跨线程累加） ----------
    {
        sylar::Mutex mu;
        long counter = 0;
        const int kThreads = 4;
        const int kIters = 20000;

        std::vector<Thread::ptr> ths;
        for (int t = 0; t < kThreads; ++t) {
            ths.push_back(Thread::ptr(new Thread([&]() {
                for (int k = 0; k < kIters; ++k) {
                    sylar::Mutex::Lock lock(mu);
                    ++counter;
                }
            }, "counter")));
        }
        for (auto& th : ths) {
            th->join();
        }

        long expect = (long)kThreads * kIters;
        printf("mutex counter=%ld expect=%ld\n", counter, expect);
        if (counter != expect) {
            printf("FAIL: Mutex 未能保护临界区（丢更新），counter=%ld expect=%ld\n",
                   counter, expect);
            fail = 1;
        }
    }

    // ---------- 3) Thread：线程名传递与 SetName ----------
    {
        std::string seen;
        sylar::Mutex nmu;
        Thread th([&]() {
            {
                sylar::Mutex::Lock l(nmu);
                seen = Thread::GetName();     // run() 已把 m_name 写入 thread_local
            }
            Thread::SetName("renamed");
            {
                sylar::Mutex::Lock l(nmu);
                seen += "/" + Thread::GetName();
            }
        }, "probe");
        th.join();

        printf("thread name probe: %s\n", seen.c_str());
        if (seen != "probe/renamed") {
            printf("FAIL: thread name expected 'probe/renamed', got '%s'\n", seen.c_str());
            fail = 1;
        }
    }

    if (fail) {
        printf("SOME CHECKS FAILED\n");
        return 1;
    }
    printf("PASS: multi-thread IOManager, recurring timer, mutex & thread name\n");
    return 0;
}
