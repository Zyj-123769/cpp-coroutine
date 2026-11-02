// 协程版 echo server 示例（Phase 5）
// 演示：同步的 read/write/accept 写法，配合 hook + IOScheduler 支撑高并发
#include "iomanager.h"
#include "hook.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace sylar;

// 处理单个连接：读取并原样回写
static void handle_client(int fd) {
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));   // 协程级阻塞
        if (n <= 0) {
            break;
        }
        // 处理 write 的部分写：循环直至全部写出或出错
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(fd, buf + off, n - off);   // 协程级阻塞
            if (w <= 0) {
                break;
            }
            off += (size_t)w;
        }
        printf("echo %zd bytes to fd=%d\n", n, fd);
    }
    close(fd);
    printf("connection fd=%d closed\n", fd);
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8000;

    IOManager iom(1, true, "echo");

    // 在调度线程内创建监听 socket（socket 被 hook -> 自动 O_NONBLOCK）
    iom.schedule([&]() {
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0 ||
            listen(listen_fd, 1024) < 0) {
            perror("bind/listen");
            return;
        }
        printf("echo server listening on 0.0.0.0:%d\n", port);

        while (true) {
            sockaddr_in caddr;
            socklen_t len = sizeof(caddr);
            int cfd = accept(listen_fd, (sockaddr*)&caddr, &len);   // 协程级阻塞
            if (cfd < 0) {
                continue;
            }
            printf("new connection fd=%d\n", cfd);
            // 每个连接一个协程处理
            iom.schedule(std::bind(handle_client, cfd));
        }
    });

    // 服务长驻：accept 协程始终有 pending 事件，stop() 会阻塞运行直到进程被终止（Ctrl+C）
    iom.stop();
    return 0;
}