// 边界测试：协程栈溢出时必须「明确报错」而非静默踩内存
// 用 fork 子进程执行溢出，父进程断言：
//   - 原生构建：保护页触发 SIGSEGV
//   - ASan 构建：ASan 自行检测并输出 stack-overflow 报告
#include "fiber.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace sylar;

static const char kChildLog[] = "/tmp/stack_overflow_child.log";

// 深度递归消耗栈。
//
// 关键约束：这里必须是「非尾递归」。
//   若写成 `recurse(depth - 1, sink); return;`，这是纯尾调用，clang 在 -O1 就会启用
//   sibling-call 优化把它折叠成一个循环 —— 栈帧不再增长，递归可以「跑完」而不溢出，
//   子进程正常退出，测试反而误报 FAIL。历史上 CI 的 clang 18 正是如此
//   （gcc 未折叠，所以该失败只在 clang 侧暴露）。
//   因此：递归调用之后继续使用返回值与 pad，使调用不再是尾调用；
//   并且 noinline 防止被内联后重新形成尾调用。
__attribute__((noinline))
static int recurse(int depth, volatile char* sink) {
    volatile char pad[64];
    pad[0] = (char)(depth & 0x7f);   // 强制占用栈帧
    *sink = (char)(pad[0] + depth);
    if (depth <= 0) {
        return pad[0];
    }
    int sub = recurse(depth - 1, sink);   // 非尾调用：返回值需在返回后继续使用
    pad[1] = (char)(sub & 0x7f);          // 调用后仍使用 pad，保证栈帧跨调用存活
    *sink = (char)(pad[1] + sub);
    return sub + 1;
}

int main() {
    unlink(kChildLog);

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：把 stderr 重定向到日志文件（ASan 报告会写到 stderr）
        int lfd = open(kChildLog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lfd >= 0) {
            dup2(lfd, STDERR_FILENO);
            close(lfd);
        }
        // 16KB 栈上深度递归，预期踩到保护页 / 触发 ASan 检测
        Fiber::GetThis();
        volatile char sink = 0;
        Fiber::ptr f(new Fiber([&]() {
            printf("  [child] recursion on 16KB stack, expect crash...\n");
            fflush(stdout);
            recurse(1 << 24, &sink);   // 极深递归，必溢出
            printf("  [child] recursion returned (unexpected!)\n");
            fflush(stdout);
            _exit(3);   // 递归跑完却没溢出：说明递归被优化成循环了，测试本身失效
        }, 16 * 1024, true));
        f->call();
        printf("  [child] fiber returned (unexpected!)\n");
        fflush(stdout);
        _exit(4);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    bool clear_error = false;
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV) {
        clear_error = true;   // 保护页：硬件 SIGSEGV
    }
    // ASan：自行检测到栈溢出并以报告终止
    FILE* fp = fopen(kChildLog, "r");
    if (fp) {
        char buf[1024] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        if (n > 0 && (strstr(buf, "stack-overflow") || strstr(buf, "stack-buffer-overflow"))) {
            clear_error = true;
        }
    }

    if (clear_error) {
        printf("PASS: stack overflow -> clear error (SIGSEGV guard page / ASan report), no silent corruption\n");
        return 0;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 3) {
        printf("FAIL: recursion returned without overflowing — 递归被优化器折叠成循环"
               "（尾调用/sibling-call 优化），本测试已失去意义\n");
        return 1;
    }
    printf("FAIL: expected clear stack-overflow error, got status=0x%x\n", status);
    return 1;
}