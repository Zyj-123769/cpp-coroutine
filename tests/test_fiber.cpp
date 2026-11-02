// Phase 1 演示：Fiber 与 ucontext 上下文切换
// 演示主协程与子协程交替输出 MAIN -> SUB -> MAIN -> SUB
#include "fiber.h"
#include <cstdio>

using namespace sylar;

int main() {
    // 1. 创建当前线程的主协程（保存线程真实上下文）
    Fiber::GetThis();

    // 2. 创建子协程
    //    use_caller=true：让它在让出/结束时切回线程主协程（无需调度器即可单独验证）
    Fiber::ptr sub(new Fiber([]() {
        printf("  SUB #1\n");
        Fiber::GetThis()->back();   // 让出，切回主协程
        printf("  SUB #2\n");
    }, 0, /*use_caller=*/true));

    // 3. 主协程与子协程交替执行
    printf("MAIN #1\n");
    sub->call();        // 进入子协程 -> 子协程打印 SUB #1 后 back 回来
    printf("MAIN #2\n");
    sub->call();        // 再次进入 -> 打印 SUB #2 后结束
    printf("MAIN #3\n");

    printf("test_fiber done, total fibers=%llu\n",
           (unsigned long long)Fiber::TotalFibers());
    return 0;
}