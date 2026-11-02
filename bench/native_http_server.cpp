// 压测用：原生线程 HTTP server（每连接一线程），作为协程版的对比基线
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

static const char kResp[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n"
    "\r\n"
    "hello world";

struct Conn {
    int fd;
};

static void* handle(void* arg) {
    Conn* c = (Conn*)arg;
    int fd = c->fd;
    delete c;

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));   // 线程阻塞在 read 上
    if (n > 0) {
        write(fd, kResp, sizeof(kResp) - 1);
    }
    close(fd);
    return nullptr;
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8081;

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
        return 1;
    }
    printf("native thread http server on 0.0.0.0:%d\n", port);

    while (true) {
        sockaddr_in c;
        socklen_t l = sizeof(c);
        int cfd = accept(lfd, (sockaddr*)&c, &l);
        if (cfd < 0) {
            continue;
        }
        Conn* conn = new Conn{cfd};
        pthread_t t;
        pthread_create(&t, nullptr, handle, conn);
        pthread_detach(t);
    }
    return 0;
}