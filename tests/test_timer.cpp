// Phase 4 演示：Timer 最小堆定时器
// 100ms/500ms/1000ms 三个任务触发时间的毫秒级精度验证
#include "timer.h"
#include "util.h"
#include <cstdio>
#include <map>
#include <unistd.h>
#include <vector>

using namespace sylar;

// 具体定时器管理器（onTimerInsertedAtFront 为纯虚函数，这里做空实现即可）
class TestTimerManager : public TimerManager {
protected:
    void onTimerInsertedAtFront() override {}
};

int main() {
    TestTimerManager tm;
    std::map<uint64_t, uint64_t> fired{{100, 0}, {500, 0}, {1000, 0}};
    uint64_t base = GetCurrentMS();

    tm.addTimer(100,  [&]() { fired[100]  = GetCurrentMS() - base; });
    tm.addTimer(500,  [&]() { fired[500]  = GetCurrentMS() - base; });
    tm.addTimer(1000, [&]() { fired[1000] = GetCurrentMS() - base; });

    // 轮询：取堆顶到期时间 -> 睡眠 -> 取出已到期回调
    while (tm.hasTimer()) {
        uint64_t next = tm.getNextTimer();
        if (next == ~0ull || next == 0) {
            next = 1;
        }
        usleep(next * 1000);   // 真实 usleep（此处无调度器，hook 未启用，走真实实现）
        std::vector<std::function<void()>> cbs;
        tm.listExpiredCb(cbs);
        for (auto& cb : cbs) {
            cb();
        }
    }

    printf("timer 100ms  fired at %llu ms\n", (unsigned long long)fired[100]);
    printf("timer 500ms  fired at %llu ms\n", (unsigned long long)fired[500]);
    printf("timer 1000ms fired at %llu ms\n", (unsigned long long)fired[1000]);
    return 0;
}