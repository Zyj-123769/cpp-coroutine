#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include "mutex.h"
#include "thread.h"

namespace sylar {

class TimerManager;

// 定时器：记录到期时间与回调
class Timer : public std::enable_shared_from_this<Timer> {
    friend class TimerManager;
public:
    typedef std::shared_ptr<Timer> ptr;

    bool cancel();                     // 取消定时器
    bool refresh();                    // 刷新到期时间（从现在重新计时）
    bool reset(uint64_t ms, bool from_now); // 重置定时时间

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);
    Timer(uint64_t next);              // 用于 lower_bound 查找的临时节点

private:
    bool m_recurring = false;          // 是否循环
    uint64_t m_ms = 0;                 // 执行周期
    uint64_t m_next = 0;               // 精确到期时间（毫秒时间戳）
    std::function<void()> m_cb;
    TimerManager* m_manager = nullptr;

    struct Comparator {
        bool operator()(const Timer::ptr& lhs, const Timer::ptr& rhs) const;
    };
};

// 定时器管理器：最小堆（std::set 按到期时间有序）
class TimerManager {
    friend class Timer;
public:
    typedef RWMutex RWMutexType;

    TimerManager();
    virtual ~TimerManager();

    // 添加定时器（ms 毫秒后触发）
    Timer::ptr addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);
    // 添加条件定时器（weak_cond 有效时才触发回调）
    Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb,
                                 std::weak_ptr<void> weak_cond, bool recurring = false);

    // 到最近一个定时器到期的剩余毫秒数（无定时器返回 ~0ull）
    uint64_t getNextTimer();
    // 取出所有已到期定时器的回调
    void listExpiredCb(std::vector<std::function<void()>>& cbs);
    bool hasTimer();

protected:
    virtual void onTimerInsertedAtFront() = 0;
    void addTimer(Timer::ptr val, RWMutexType::WriteLock& lock);

private:
    RWMutexType m_mutex;
    std::set<Timer::ptr, Timer::Comparator> m_timers;
    bool m_tickled = false;
};

} // namespace sylar