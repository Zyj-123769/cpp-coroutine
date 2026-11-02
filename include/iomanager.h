#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "scheduler.h"
#include "timer.h"

namespace sylar {

// 基于 epoll 的 IO 协程调度器（集成定时器）
class IOManager : public Scheduler, public TimerManager {
public:
    typedef std::shared_ptr<IOManager> ptr;
    typedef RWMutex RWMutexType;

    enum Event {
        NONE  = 0x0,
        READ  = 0x1,   // EPOLLIN
        WRITE = 0x4,   // EPOLLOUT
    };

private:
    struct FdContext {
        typedef Mutex MutexType;

        struct EventContext {
            Scheduler* scheduler = nullptr;   // 事件所属调度器
            Fiber::ptr fiber;                 // 等待事件的协程
            std::function<void()> cb;         // 或一个回调
        };

        EventContext& getContext(Event event);
        void resetContext(EventContext& ctx);
        void triggerEvent(Event event);

        EventContext read;
        EventContext write;
        int fd = 0;
        Event events = NONE;
        MutexType mutex;
    };

public:
    IOManager(size_t threads = 1, bool use_caller = true, const std::string& name = "");
    ~IOManager();

    // 添加事件：事件就绪时调度 cb 或当前协程
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    // 删除事件：仅移除注册，不唤醒等待者（语义是「放弃等待」）。
    // 若等待者仍挂在该事件上且需要唤醒（如超时/取消），请使用 cancelEvent / cancelAll。
    bool delEvent(int fd, Event event);
    // 取消事件：移除注册并唤醒等待者
    bool cancelEvent(int fd, Event event);
    bool cancelAll(int fd);

    static IOManager* GetThis();

protected:
    void tickle() override;
    bool stopping() override;
    void idle() override;
    void onTimerInsertedAtFront() override;

    void contextResize(size_t size);
    bool stopping(uint64_t& timeout);

private:
    int m_epfd = 0;
    int m_tickleFds[2];                              // 唤醒管道
    std::atomic<size_t> m_pendingEventCount = {0};   // 待处理事件数
    RWMutexType m_mutex;
    std::vector<FdContext*> m_fdContexts;
};

} // namespace sylar