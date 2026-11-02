#include "fd_manager.h"
#include "hook.h"
#include "log.h"
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace sylar {

FdCtx::FdCtx(int fd)
    : m_isInit(false)
    , m_isSocket(false)
    , m_sysNonblock(false)
    , m_userNonblock(false)
    , m_isClosed(false)
    , m_fd(fd)
    , m_recvTimeout(-1)
    , m_sendTimeout(-1) {
    init();
}

FdCtx::~FdCtx() {
}

bool FdCtx::init() {
    if (m_isInit) {
        return true;
    }
    m_recvTimeout = -1;
    m_sendTimeout = -1;

    struct stat fd_stat;
    if (-1 == fstat(m_fd, &fd_stat)) {
        m_isInit = false;
        m_isSocket = false;
    } else {
        m_isInit = true;
        m_isSocket = S_ISSOCK(fd_stat.st_mode);
    }

    if (m_isSocket) {
        // 必须把 fd 设成非阻塞：do_io 的非阻塞试探（read 返回 EAGAIN 才挂起协程）
        // 完全建立在这一步成功的前提上。若这里静默失败仍置 m_sysNonblock = true，
        // 之后 read 会真的阻塞住，把整个调度线程卡死 —— 所以失败时要如实记录。
        int flags = fcntl_f(m_fd, F_GETFL, 0);
        if (flags == -1) {
            LOG_ERROR() << "FdCtx::init F_GETFL failed, fd=" << m_fd << " errno=" << errno;
            m_sysNonblock = false;
        } else if (!(flags & O_NONBLOCK) &&
                   fcntl_f(m_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            LOG_ERROR() << "FdCtx::init F_SETFL(O_NONBLOCK) failed, fd=" << m_fd
                        << " errno=" << errno;
            m_sysNonblock = false;
        } else {
            m_sysNonblock = true;
        }
    } else {
        m_sysNonblock = false;
    }

    m_userNonblock = false;
    m_isClosed = false;
    return m_isInit;
}

void FdCtx::setTimeout(int type, uint64_t v) {
    if (type == SO_RCVTIMEO) {
        m_recvTimeout = v;
    } else {
        m_sendTimeout = v;
    }
}

uint64_t FdCtx::getTimeout(int type) {
    if (type == SO_RCVTIMEO) {
        return m_recvTimeout;
    }
    return m_sendTimeout;
}

FdManager::FdManager() {
    m_datas.resize(64);
}

FdCtx::ptr FdManager::get(int fd, bool auto_create) {
    if (fd == -1) {
        return nullptr;
    }
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_datas.size() <= fd) {
        if (!auto_create) {
            return nullptr;
        }
    } else {
        if (m_datas[fd] || !auto_create) {
            return m_datas[fd];
        }
    }
    lock.unlock();

    RWMutexType::WriteLock lock2(m_mutex);
    // 二次检查：其它线程可能已并发创建该 fd 的 FdCtx
    if ((int)m_datas.size() > fd && m_datas[fd]) {
        return m_datas[fd];
    }
    FdCtx::ptr ctx(new FdCtx(fd));
    if (fd >= (int)m_datas.size()) {
        // 扩容必须同时满足两点：至少容纳 fd 本身（fd + 1），且尽量按 1.5 倍增长以减少
        // 反复 resize。原写法 resize(fd * 1.5) 在语义上依赖「fd 恰好大于 size」的隐含
        // 前提，读者容易怀疑边界，这里显式取二者较大值。
        size_t need = (size_t)fd + 1;
        size_t grow = (size_t)(m_datas.size() * 1.5);
        m_datas.resize(need > grow ? need : grow);
    }
    m_datas[fd] = ctx;
    return ctx;
}

void FdManager::del(int fd) {
    RWMutexType::WriteLock lock(m_mutex);
    if ((int)m_datas.size() <= fd) {
        return;
    }
    m_datas[fd].reset();
}

} // namespace sylar