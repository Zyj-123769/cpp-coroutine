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

// 深度递归消耗栈（volatile + 局部数组防优化）
static void recurse(int depth, volatile char* sink) {
    volatile char pad[64];
    pad[0] = (char)(depth & 0x7f);   // 强制占用栈帧
    *sink = (char)(pad[0] + depth);
    if (depth <= 0) {
        return;
    }
    recurse(depth - 1, sink);
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
            recurse(1 << 24, &sink);   // 极深递归，必溢出
            printf("  [child] recursion returned (unexpected!)\n");
        }, 16 * 1024, true));
        f->call();
        printf("  [child] fiber returned (unexpected!)\n");
        _exit(0);
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
    printf("FAIL: expected clear stack-overflow error, got status=0x%x\n", status);
    return 1;
}