#include "hook.h"
#include "fd_manager.h"
#include "fiber.h"
#include "iomanager.h"
#include "log.h"
#include "macro.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

namespace sylar {

static thread_local bool t_hook_enable = false;

#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt)

// 初始化：通过 dlsym(RTLD_NEXT) 取得系统真实函数地址
void hook_init() {
    static bool is_inited = false;
    if (is_inited) {
        return;
    }
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
    HOOK_FUN(XX);
#undef XX
}

static uint64_t s_connect_timeout = (uint64_t)-1;

struct _HookIniter {
    _HookIniter() {
        hook_init();
    }
};
static _HookIniter s_hook_initer;

bool is_hook_enable() {
    return t_hook_enable;
}

void set_hook_enable(bool flag) {
    t_hook_enable = flag;
}

} // namespace sylar

// 定时器超时信息（供 do_io / connect 使用）
struct timer_info {
    int cancelled = 0;
};

// 通用 IO 挂起模板：非阻塞尝试 -> 未就绪则挂到 IOManager 的 fd 事件并 yield
template<typename OriginFun, typename... Args>
static ssize_t do_io(int fd, OriginFun fun, const char* hook_fun_name,
                     uint32_t event, int timeout_so, Args&&... args) {
    if (!sylar::t_hook_enable) {
        return fun(fd, std::forward<Args>(args)...);
    }

    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if (!ctx) {
        return fun(fd, std::forward<Args>(args)...);
    }

    if (ctx->isClose()) {
        errno = EBADF;
        return -1;
    }

    if (!ctx->isSocket() || ctx->getUserNonblock()) {
        return fun(fd, std::forward<Args>(args)...);
    }

    uint64_t to = ctx->getTimeout(timeout_so);
    std::shared_ptr<timer_info> tinfo(new timer_info);

retry:
    ssize_t n = fun(fd, std::forward<Args>(args)...);
    while (n == -1 && errno == EINTR) {
        n = fun(fd, std::forward<Args>(args)...);
    }
    if (n == -1 && errno == EAGAIN) {
        sylar::IOManager* iom = sylar::IOManager::GetThis();
        // 纯 Scheduler（无 epoll）下无法挂起协程，直接返回 EAGAIN 交由调用方处理
        if (SYLAR_UNLIKELY(!iom)) {
            return n;
        }
        sylar::Timer::ptr timer;
        std::weak_ptr<timer_info> winfo(tinfo);

        // 若设置了超时，则注册条件定时器来取消 IO 事件
        if (to != (uint64_t)-1) {
            timer = iom->addConditionTimer(to, [winfo, fd, iom, event]() {
                auto t = winfo.lock();
                if (!t || t->cancelled) {
                    return;
                }
                t->cancelled = ETIMEDOUT;
                iom->cancelEvent(fd, (sylar::IOManager::Event)(event));
            }, winfo);
        }

        int rt = iom->addEvent(fd, (sylar::IOManager::Event)(event));
        if (SYLAR_UNLIKELY(rt)) {
            LOG_ERROR() << hook_fun_name << " addEvent(" << fd << ", " << event << ") error";
            if (timer) {
                timer->cancel();
            }
            return -1;
        } else {
            sylar::Fiber::YieldToHold();
            if (timer) {
                timer->cancel();
            }
            if (tinfo->cancelled) {
                errno = tinfo->cancelled;
                return -1;
            }
            goto retry;
        }
    }
    return n;
}

extern "C" {

#define XX(name) name##_fun name##_f = nullptr;
    HOOK_FUN(XX);
#undef XX

unsigned int sleep(unsigned int seconds) {
    if (!sylar::t_hook_enable) {
        return sleep_f(seconds);
    }
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();
    if (!iom) {
        return sleep_f(seconds);
    }
    // 睡眠到点时重新调度当前协程
    iom->addTimer(seconds * 1000, [iom, fiber]() {
        iom->schedule(fiber);
    });
    sylar::Fiber::YieldToHold();
    return 0;
}

int usleep(useconds_t usec) {
    if (!sylar::t_hook_enable) {
        return usleep_f(usec);
    }
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();
    if (!iom) {
        return usleep_f(usec);
    }
    // 向上取整到毫秒，保证休眠时长 >= 请求值（否则 usleep(500) 会变成 0ms 立即返回）
    iom->addTimer((usec + 999) / 1000, [iom, fiber]() {
        iom->schedule(fiber);
    });
    sylar::Fiber::YieldToHold();
    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!sylar::t_hook_enable) {
        return nanosleep_f(req, rem);
    }
    // 向上取整到毫秒并使用 uint64 避免 tv_sec 较大时溢出；保证休眠时长 >= 请求值
    uint64_t timeout_ms = (uint64_t)req->tv_sec * 1000 + (req->tv_nsec + 999999) / 1000000;
    sylar::Fiber::ptr fiber = sylar::Fiber::GetThis();
    sylar::IOManager* iom = sylar::IOManager::GetThis();
    if (!iom) {
        return nanosleep_f(req, rem);
    }
    iom->addTimer(timeout_ms, [iom, fiber]() {
        iom->schedule(fiber);
    });
    sylar::Fiber::YieldToHold();
    return 0;
}

int socket(int domain, int type, int protocol) {
    if (!sylar::t_hook_enable) {
        return socket_f(domain, type, protocol);
    }
    int fd = socket_f(domain, type, protocol);
    if (fd == -1) {
        return fd;
    }
    // 注册 FdCtx 并设置 O_NONBLOCK
    sylar::FdMgr::GetInstance()->get(fd, true);
    return fd;
}

int connect_with_timeout(int fd, const struct sockaddr* addr, socklen_t addrlen, uint64_t timeout_ms) {
    if (!sylar::t_hook_enable) {
        return connect_f(fd, addr, addrlen);
    }
    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if (!ctx || ctx->isClose()) {
        errno = EBADF;
        return -1;
    }
    if (!ctx->isSocket() || ctx->getUserNonblock()) {
        return connect_f(fd, addr, addrlen);
    }

    int n = connect_f(fd, addr, addrlen);
    if (n == 0) {
        return 0;
    } else if (n != -1 || errno != EINPROGRESS) {
        return n;
    }

    sylar::IOManager* iom = sylar::IOManager::GetThis();
    // 无 IOManager 时无法异步等待连接建立，返回 EINPROGRESS 交由调用方处理
    if (!iom) {
        return n;
    }
    sylar::Timer::ptr timer;
    std::shared_ptr<timer_info> tinfo(new timer_info);
    std::weak_ptr<timer_info> winfo(tinfo);

    if (timeout_ms != (uint64_t)-1) {
        timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]() {
            auto t = winfo.lock();
            if (!t || t->cancelled) {
                return;
            }
            t->cancelled = ETIMEDOUT;
            iom->cancelEvent(fd, sylar::IOManager::WRITE);
        }, winfo);
    }

    int rt = iom->addEvent(fd, sylar::IOManager::WRITE);
    if (rt == 0) {
        sylar::Fiber::YieldToHold();
        if (timer) {
            timer->cancel();
        }
        if (tinfo->cancelled) {
            errno = tinfo->cancelled;
            return -1;
        }
    } else {
        if (timer) {
            timer->cancel();
        }
        LOG_ERROR() << "connect addEvent(" << fd << ", WRITE) error";
        return -1;
    }

    int error = 0;
    socklen_t len = sizeof(int);
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len)) {
        return -1;
    }
    if (!error) {
        return 0;
    }
    errno = error;
    return -1;
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    return connect_with_timeout(sockfd, addr, addrlen, sylar::s_connect_timeout);
}

int accept(int s, struct sockaddr* addr, socklen_t* addrlen) {
    int fd = do_io(s, accept_f, "accept", sylar::IOManager::READ, SO_RCVTIMEO, addr, addrlen);
    if (fd >= 0) {
        sylar::FdMgr::GetInstance()->get(fd, true);
    }
    return fd;
}

ssize_t read(int fd, void* buf, size_t count) {
    return do_io(fd, read_f, "read", sylar::IOManager::READ, SO_RCVTIMEO, buf, count);
}

ssize_t readv(int fd, const struct iovec* iov, int iovcnt) {
    return do_io(fd, readv_f, "readv", sylar::IOManager::READ, SO_RCVTIMEO, iov, iovcnt);
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    return do_io(sockfd, recv_f, "recv", sylar::IOManager::READ, SO_RCVTIMEO, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, socklen_t* addrlen) {
    return do_io(sockfd, recvfrom_f, "recvfrom", sylar::IOManager::READ, SO_RCVTIMEO,
                 buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr* msg, int flags) {
    return do_io(sockfd, recvmsg_f, "recvmsg", sylar::IOManager::READ, SO_RCVTIMEO, msg, flags);
}

ssize_t write(int fd, const void* buf, size_t count) {
    return do_io(fd, write_f, "write", sylar::IOManager::WRITE, SO_SNDTIMEO, buf, count);
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    return do_io(fd, writev_f, "writev", sylar::IOManager::WRITE, SO_SNDTIMEO, iov, iovcnt);
}

ssize_t send(int s, const void* msg, size_t len, int flags) {
    return do_io(s, send_f, "send", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, len, flags);
}

ssize_t sendto(int s, const void* msg, size_t len, int flags,
               const struct sockaddr* to, socklen_t tolen) {
    return do_io(s, sendto_f, "sendto", sylar::IOManager::WRITE, SO_SNDTIMEO,
                 msg, len, flags, to, tolen);
}

ssize_t sendmsg(int s, const struct msghdr* msg, int flags) {
    return do_io(s, sendmsg_f, "sendmsg", sylar::IOManager::WRITE, SO_SNDTIMEO, msg, flags);
}

int close(int fd) {
    if (!sylar::t_hook_enable) {
        return close_f(fd);
    }
    sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
    if (ctx) {
        auto iom = sylar::IOManager::GetThis();
        if (iom) {
            iom->cancelAll(fd);
        }
        sylar::FdMgr::GetInstance()->del(fd);
    }
    return close_f(fd);
}

int fcntl(int fd, int cmd, ...) {
    va_list va;
    va_start(va, cmd);
    switch (cmd) {
        case F_SETFL: {
            int arg = va_arg(va, int);
            va_end(va);
            // 仅在 hook 启用时做 FdMgr 簿记。libc/ASan 等运行时内部也会调用 fcntl，
            // 若在此无条件进入 FdMgr::get() 会造成重入死锁（get 内部持有锁）。
            if (!sylar::t_hook_enable) {
                return fcntl_f(fd, cmd, arg);
            }
            sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
            if (!ctx || ctx->isClose() || !ctx->isSocket()) {
                return fcntl_f(fd, cmd, arg);
            }
            ctx->setUserNonblock(arg & O_NONBLOCK);
            if (ctx->getSysNonblock()) {
                arg |= O_NONBLOCK;
            } else {
                arg &= ~O_NONBLOCK;
            }
            return fcntl_f(fd, cmd, arg);
        }
        case F_GETFL: {
            va_end(va);
            int arg = fcntl_f(fd, cmd);
            if (!sylar::t_hook_enable) {
                return arg;
            }
            sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(fd);
            if (!ctx || ctx->isClose() || !ctx->isSocket()) {
                return arg;
            }
            if (ctx->getUserNonblock()) {
                return arg | O_NONBLOCK;
            }
            return arg & ~O_NONBLOCK;
        }
        case F_DUPFD:
        case F_DUPFD_CLOEXEC:
        case F_SETFD:
        case F_SETOWN:
        case F_SETSIG:
        case F_SETLEASE:
        case F_NOTIFY:
#ifdef F_SETPIPE_SZ
        case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
        case F_GETFD:
        case F_GETOWN:
        case F_GETSIG:
        case F_GETLEASE:
#ifdef F_GETPIPE_SZ
        case F_GETPIPE_SZ:
#endif
            {
                // 无参命令：不能 va_arg，否则为未定义行为
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;
        case F_SETLK:
        case F_SETLKW:
        case F_GETLK:
            {
                struct flock* arg = va_arg(va, struct flock*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
#ifdef F_GETOWN_EX
        case F_GETOWN_EX:
        case F_SETOWN_EX:
            {
                void* arg = va_arg(va, void*);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
#endif
        default:
            va_end(va);
            return fcntl_f(fd, cmd);
    }
}

int ioctl(int d, unsigned long int request, ...) {
    va_list va;
    va_start(va, request);
    void* arg = va_arg(va, void*);
    va_end(va);

    if (FIONBIO == request) {
        bool user_nonblock = !!*(int*)arg;
        sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(d);
        if (!ctx || ctx->isClose() || !ctx->isSocket()) {
            return ioctl_f(d, request, arg);
        }
        ctx->setUserNonblock(user_nonblock);
    }
    return ioctl_f(d, request, arg);
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
    return getsockopt_f(sockfd, level, optname, optval, optlen);
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    if (!sylar::t_hook_enable) {
        return setsockopt_f(sockfd, level, optname, optval, optlen);
    }
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            sylar::FdCtx::ptr ctx = sylar::FdMgr::GetInstance()->get(sockfd);
            if (ctx) {
                const timeval* v = (const timeval*)optval;
                ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
            }
        }
    }
    return setsockopt_f(sockfd, level, optname, optval, optlen);
}

} // extern "C"