// 压测用：协程版 HTTP server（hook 使同步写法异步化，单线程支撑高并发）
#include "iomanager.h"
#include "hook.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace sylar;

static const char kResp[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello world";

// 处理连接：读一次请求 -> 回固定响应 -> 关闭（单请求一连接，ab 默认模式）
static void handle(int fd) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));   // 协程级阻塞
    if (n > 0) {
        // 处理 write 的部分写：循环直至全部写出或出错
        size_t total = sizeof(kResp) - 1;
        size_t off = 0;
        while (off < total) {
            ssize_t w = write(fd, kResp + off, total - off);   // 协程级阻塞
            if (w <= 0) {
                break;
            }
            off += (size_t)w;
        }
    }
    close(fd);
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;

    IOManager iom(1, true, "http");

    iom.schedule([&]() {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons(port);

        if (bind(lfd, (sockaddr*)&a, sizeof(a)) < 0 || listen(lfd, 1024) < 0) {
            perror("bind/listen");
            return;
        }
        printf("coroutine http server on 0.0.0.0:%d\n", port);

        while (true) {
            sockaddr_in c;
            socklen_t l = sizeof(c);
            int cfd = accept(lfd, (sockaddr*)&c, &l);   // 协程级阻塞
            if (cfd < 0) {
                continue;
            }
            // 每连接一个协程
            iom.schedule(std::bind(handle, cfd));
        }
    });

    iom.stop();   // 长驻，Ctrl+C 退出
    return 0;
}