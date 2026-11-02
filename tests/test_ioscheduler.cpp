// Phase 3 演示：IOScheduler（epoll 事件驱动）
// read 在协程内阻塞等待但不占 CPU，数据到达后唤醒继续
#include "fd_manager.h"
#include "hook.h"
#include "iomanager.h"
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

using namespace sylar;

int main() {
    // 用 socketpair 创建一对已连接 socket（do_io 只对 socket 类 fd 做协程挂起，
    // 若用 pipe 则 fd 无 FdCtx、会退回真实阻塞 read，导致单线程调度器死锁）。
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        return 1;
    }
    // socketpair 未被 hook，需手动注册 FdCtx 并置 O_NONBLOCK，
    // 这样 read/write 才会被 do_io 接管（挂起协程而非阻塞线程）。
    FdMgr::GetInstance()->get(fds[0], true);
    FdMgr::GetInstance()->get(fds[1], true);

    IOManager iom(1, true, "io");

    // 读协程：协程级阻塞读，数据到达后唤醒（read 被 hook）
    iom.schedule([&]() {
        char buf[32] = {0};
        printf("[reader] start, waiting data...\n");
        ssize_t n = read(fds[0], buf, sizeof(buf));   // 挂起到 epoll，不忙等
        printf("[reader] got %zd bytes: %s\n", n, buf);
    });

    // 写协程：100ms 后写入数据（usleep 被 hook 成定时器，同样不忙等）
    iom.schedule([&]() {
        usleep(100 * 1000);
        const char* msg = "hello";
        write(fds[1], msg, 5);
        printf("[writer] sent data\n");
    });

    iom.stop();

    close(fds[0]);
    close(fds[1]);
    printf("test_ioscheduler done\n");
    return 0;
}