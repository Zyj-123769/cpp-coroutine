// 边界测试：协程回调抛异常时被 MainFunc 捕获，不扩散、不崩溃，调度器继续工作
#include "iomanager.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

using namespace sylar;

int main() {
    std::atomic<int> done{0};
    IOManager iom(1, true, "except");

    // 1) 抛异常的协程（std::exception 子类）
    iom.schedule([]() {
        printf("task[throw1] start\n");
        throw std::runtime_error("boom");
    });

    // 2) 异常后投递的正常任务必须照常执行
    iom.schedule([&]() {
        printf("task[normal] runs after exception\n");
        done.fetch_add(1);
    });

    // 3) 再抛一个（异常恢复能力）
    iom.schedule([]() {
        throw std::logic_error("boom2");
    });

    // 4) 收尾任务
    iom.schedule([&]() {
        printf("task[final] runs\n");
        done.fetch_add(1);
    });

    iom.stop();

    if (done.load() != 2) {
        printf("FAIL: expected 2 normal tasks to run, got %d\n", done.load());
        return 1;
    }
    printf("PASS: exceptions contained, scheduler continued, total fibers=%llu\n",
           (unsigned long long)Fiber::TotalFibers());
    return 0;
}