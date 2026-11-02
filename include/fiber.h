#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <ucontext.h>

namespace sylar {

class Scheduler;

// 有栈协程类：基于 ucontext_t（makecontext/swapcontext）实现用户态上下文切换
class Fiber : public std::enable_shared_from_this<Fiber> {
    friend class Scheduler;

public:
    typedef std::shared_ptr<Fiber> ptr;

    enum State {
        INIT = 0,   // 初始状态
        HOLD,       // 挂起（等待事件）
        EXEC,       // 正在执行
        TERM,       // 结束
        READY,      // 就绪（可被重新调度）
        EXCEPT,     // 异常结束
    };

    // 子协程构造：cb 入口、stacksize 栈大小(0 用默认 128KB)、use_caller 是否为调度协程
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool use_caller = false);
    ~Fiber();

    // 复用协程（重置回调，仅限 INIT/TERM/EXCEPT 状态）
    void reset(std::function<void()> cb);

    // 调度器语境下切换：当前 -> 本协程 / 本协程 -> 调度主协程
    void swapIn();
    void swapOut();

    // 无调度器语境下切换（基于线程真实主协程）
    void call();
    void back();

    uint64_t getId() const { return m_id; }
    State getState() const { return m_state; }

    // 栈信息：ASan 协程栈切换标注需要（主协程返回 nullptr / 0，表示线程自身栈）
    const void* getStackBottom() const { return m_stack; }
    size_t getStackSize() const { return m_stacksize; }

    static void SetThis(Fiber* f);
    static Fiber::ptr GetThis();
    static void YieldToReady();
    static void YieldToHold();
    static uint64_t TotalFibers();
    static uint64_t GetFiberId();

    // 子协程入口桩：执行回调后切回调度主协程
    static void MainFunc();
    // 调度协程入口桩：执行 Scheduler::run 后切回线程主协程
    static void CallerMainFunc();

private:
    // 主协程私有构造（保存线程真实上下文）
    Fiber();

private:
    uint64_t m_id = 0;
    uint32_t m_stacksize = 0;
    State m_state = INIT;
    ucontext_t m_ctx;
    void* m_stack = nullptr;
    std::function<void()> m_cb;
    void* m_fake_stack = nullptr;   // ASan 协程假栈槽位（仅 sanitizer 构建使用）
};

} // namespace sylar