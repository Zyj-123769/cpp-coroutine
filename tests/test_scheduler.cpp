// Phase 2 演示：Scheduler 调度器（多任务轮转 + 多线程）
#include "scheduler.h"
#include "util.h"
#include <cstdio>

using namespace sylar;

int main() {
    // 2 个调度线程：1 个调用线程 + 1 个新线程
    Scheduler sc(2, true, "test");

    // 投递 5 个任务协程，每个任务 yield 多次，观察轮转执行
    for (int i = 0; i < 5; ++i) {
        sc.schedule([i]() {
            for (int j = 0; j < 3; ++j) {
                printf("task[%d] step=%d (fiber_id=%llu, tid=%d)\n",
                       i, j,
                       (unsigned long long)Fiber::GetFiberId(),
                       (int)GetThreadId());
                Fiber::YieldToReady();   // 让出并重新入队，实现协作式轮转
            }
        });
    }

    sc.stop();

    printf("test_scheduler done, total fibers=%llu\n",
           (unsigned long long)Fiber::TotalFibers());
    return 0;
}