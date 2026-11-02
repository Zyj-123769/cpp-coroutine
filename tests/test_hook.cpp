// Phase 5 演示：Hook 系统调用
// sleep(1) 在协程内不阻塞整个线程，其它协程仍能立即执行
#include "iomanager.h"
#include "hook.h"
#include "util.h"
#include <cstdio>

using namespace sylar;

int main() {
    IOManager iom(1, true, "hook");
    uint64_t start = GetCurrentMS();

    // A：睡 1 秒（被 hook 成定时器 + yield，不会阻塞线程）
    iom.schedule([&]() {
        uint64_t t0 = GetCurrentMS();
        printf("[A] sleep 1s, t=%llu ms\n", (unsigned long long)(t0 - start));
        sleep(1);
        printf("[A] wake, elapsed=%llu ms\n", (unsigned long long)(GetCurrentMS() - t0));
    });

    // B：应立刻执行（证明线程未被 A 的 sleep 阻塞）
    iom.schedule([&]() {
        printf("[B] run immediately, t=%llu ms\n", (unsigned long long)(GetCurrentMS() - start));
    });

    iom.stop();

    printf("test_hook done, total elapsed=%llu ms\n",
           (unsigned long long)(GetCurrentMS() - start));
    return 0;
}