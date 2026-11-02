#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include "fiber.h"
#include "mutex.h"
#include "thread.h"

namespace sylar {

// 协作式协程调度器：N 个线程 M 个协程，就绪队列 + 每线程三类上下文
class Scheduler {
public:
    typedef std::shared_ptr<Scheduler> ptr;
    typedef Mutex MutexType;

    Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name = "");
    virtual ~Scheduler();

    const std::string& getName() const { return m_name; }

    static Scheduler* GetThis();
    static Fiber* GetMainFiber();

    void start();
    void stop();

    // 调度一个协程或回调；thread 指定绑定的调度线程 id（-1 任意线程）
    template<class FiberOrCb>
    void schedule(FiberOrCb fc, int thread = -1) {
        bool need_tickle = false;
        {
            MutexType::Lock lock(m_mutex);
            need_tickle = scheduleNoLock(fc, thread);
        }
        if (need_tickle) {
            tickle();
        }
    }

    // 批量调度
    template<class InputIterator>
    void schedule(InputIterator begin, InputIterator end) {
        bool need_tickle = false;
        {
            MutexType::Lock lock(m_mutex);
            while (begin != end) {
                need_tickle = scheduleNoLock(&*begin, -1) || need_tickle;
                ++begin;
            }
        }
        if (need_tickle) {
            tickle();
        }
    }

protected:
    virtual void tickle();
    void run();
    virtual bool stopping();
    virtual void idle();
    void setThis();
    bool hasIdleThreads() const { return m_idleThreadCount > 0; }

private:
    template<class FiberOrCb>
    bool scheduleNoLock(FiberOrCb fc, int thread) {
        bool need_tickle = m_fibers.empty();
        FiberAndThread ft(fc, thread);
        if (ft.fiber || ft.cb) {
            m_fibers.push_back(ft);
        }
        return need_tickle;
    }

private:
    // 任务单元：协程或回调（回调会包装成协程）
    struct FiberAndThread {
        Fiber::ptr fiber;
        std::function<void()> cb;
        int thread;

        FiberAndThread(Fiber::ptr f, int thr) : fiber(f), thread(thr) {}
        FiberAndThread(Fiber::ptr* f, int thr) : thread(thr) { fiber.swap(*f); }
        FiberAndThread(std::function<void()> f, int thr) : cb(f), thread(thr) {}
        FiberAndThread(std::function<void()>* f, int thr) : thread(thr) { cb.swap(*f); }
        FiberAndThread() : thread(-1) {}

        void reset() {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
    };

private:
    MutexType m_mutex;                      // 任务队列锁
    std::vector<Thread::ptr> m_threads;     // 线程池
    std::list<FiberAndThread> m_fibers;     // 就绪任务队列
    Fiber::ptr m_rootFiber;                 // use_caller 时的调度协程
    std::string m_name;                     // 调度器名称

protected:
    std::vector<int> m_threadIds;                    // 调度线程 id
    size_t m_threadCount = 0;                        // 线程数量
    std::atomic<size_t> m_activeThreadCount = {0};   // 活跃线程数
    std::atomic<size_t> m_idleThreadCount = {0};     // 空闲线程数
    bool m_stopping = true;                          // 是否正在停止
    bool m_autoStop = false;                         // 是否自动停止
    int m_rootThread = 0;                            // use_caller 线程 id（-1 表示不启用）
};

} // namespace sylar