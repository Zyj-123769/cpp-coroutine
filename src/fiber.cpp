#include "fiber.h"
#include "scheduler.h"
#include "log.h"
#include "macro.h"
#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

// ASan 支持：ucontext 切换会跳转栈，必须用 __sanitizer_start/finish_switch_fiber
// 告知 ASan 栈边界，否则协程栈访问会被误报（如 stack-buffer-overflow）
// 检测方式：GCC 在 -fsanitize=address 时定义 __SANITIZE_ADDRESS__；Clang 用
// __has_feature(address_sanitizer)。注意 __has_feature 是 Clang 专属内建——
// GCC 13+ 也认识该标识符，但仅支持有限 feature 名，若直接写进 ||/&& 表达式，
// GCC 会尝试求值 __has_feature(address_sanitizer) 并因不支持而报
// “missing binary operator” 错误。故用 #ifdef __clang__ 把 Clang 专属判断整段
// 隔离：被跳过的预处理块不会展开其中的宏，从而彻底避免 GCC 求值它。
#if defined(__SANITIZE_ADDRESS__)
#define SYLAR_HAS_ASAN 1
#endif
#if defined(__clang__)
  #if __has_feature(address_sanitizer)
  #define SYLAR_HAS_ASAN 1
  #endif
#endif

#ifdef SYLAR_HAS_ASAN
#include <sanitizer/common_interface_defs.h>
#endif

namespace sylar {

static std::atomic<uint64_t> s_fiber_id{0};
static std::atomic<uint64_t> s_fiber_count{0};

// 每线程当前运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 每线程的主协程（线程真实上下文）
static thread_local Fiber::ptr t_threadFiber = nullptr;

// 默认栈大小 128KB
static const uint32_t g_fiber_stack_size = 128 * 1024;

// 页大小（保护页长度）
static const size_t g_page_size = []() {
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096;
}();

// 栈分配器：mmap 分配「栈 + 保护页」，低地址一页设为 PROT_NONE。
// 栈向下增长，一旦越界写会立即触发 SIGSEGV（明确报错），而不是静默踩脏相邻堆内存。
// 注：md 原文建议 malloc 分配栈；保护页是满足 §7 验收项
// 「栈溢出时有明确报错而非静默踩内存」的标准做法，故采用 mmap。
class StackAllocator {
public:
    static void* Alloc(size_t size) {
        size_t total = size + g_page_size;
        void* base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            return nullptr;
        }
        if (mprotect(base, g_page_size, PROT_NONE) != 0) {
            munmap(base, total);
            return nullptr;
        }
        // 返回可用区起点（保护页之上）；栈从可用区末尾向下增长
        return static_cast<char*>(base) + g_page_size;
    }
    static void Dealloc(void* vp, size_t size) {
        if (!vp) {
            return;
        }
        void* base = static_cast<char*>(vp) - g_page_size;
        munmap(base, size + g_page_size);
    }
};

uint64_t Fiber::GetFiberId() {
    return t_fiber ? t_fiber->getId() : 0;
}

// 主协程构造：保存线程真实上下文
Fiber::Fiber() {
    m_state = EXEC;
    SetThis(this);
    if (getcontext(&m_ctx)) {
        throw std::logic_error("getcontext error");
    }
    ++s_fiber_count;
    LOG_DEBUG() << "Fiber::Fiber main";
}

// 子协程构造
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool use_caller)
    : m_id(++s_fiber_id), m_cb(cb) {
    ++s_fiber_count;
    m_stacksize = stacksize ? stacksize : g_fiber_stack_size;
    m_stack = StackAllocator::Alloc(m_stacksize);
    if (!m_stack) {
        throw std::bad_alloc();
    }
    if (getcontext(&m_ctx)) {
        StackAllocator::Dealloc(m_stack, m_stacksize);
        m_stack = nullptr;
        throw std::logic_error("getcontext error");
    }
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    if (!use_caller) {
        makecontext(&m_ctx, &Fiber::MainFunc, 0);
    } else {
        makecontext(&m_ctx, &Fiber::CallerMainFunc, 0);
    }
    LOG_DEBUG() << "Fiber::Fiber id=" << m_id;
}

Fiber::~Fiber() {
    --s_fiber_count;
    if (m_stack) {
        // 子协程：只要不再被执行（非 EXEC），其独立栈均可安全释放。
        // HOLD/READY 态的协程因引用计数归零而被析构是安全的（无人会再 resume 它），
        // 但 EXEC 态说明栈仍在被使用，属于真正错误。
        if (m_state == EXEC) {
            LOG_WARN() << "Fiber destroyed while EXEC, id=" << m_id;
        }
        StackAllocator::Dealloc(m_stack, m_stacksize);
    } else {
        // 主协程
        if (m_cb) {
            LOG_WARN() << "main fiber destroyed with callback, id=" << m_id;
        }
        if (m_state != EXEC) {
            LOG_WARN() << "main fiber destroyed in state=" << m_state << ", id=" << m_id;
        }
        Fiber* cur = t_fiber;
        if (cur == this) {
            SetThis(nullptr);
        }
    }
    LOG_DEBUG() << "Fiber::~Fiber id=" << m_id;
}

void Fiber::reset(std::function<void()> cb) {
    SYLAR_ASSERT(m_stack);
    SYLAR_ASSERT(m_state == TERM || m_state == EXCEPT || m_state == INIT);
    m_cb = cb;
    if (getcontext(&m_ctx)) {
        throw std::logic_error("getcontext error");
    }
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;
    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = INIT;
}

// 调度器语境：切入本协程（从调度主协程）
void Fiber::swapIn() {
    SYLAR_ASSERT(m_state != EXEC);
    SetThis(this);
    m_state = EXEC;
    Fiber* sched = Scheduler::GetMainFiber();
#ifdef SYLAR_HAS_ASAN
    // 当前运行在调度协程上：保存调度协程的假栈，目标为当前协程
    __sanitizer_start_switch_fiber(&sched->m_fake_stack, getStackBottom(), getStackSize());
#endif
    if (swapcontext(&sched->m_ctx, &m_ctx)) {
        throw std::logic_error("swapcontext error");
    }
#ifdef SYLAR_HAS_ASAN
    // 被切回后（当前协程重新被调度）：恢复调度协程的假栈视图
    const void* ob = nullptr;
    size_t os = 0;
    __sanitizer_finish_switch_fiber(sched->m_fake_stack, &ob, &os);
#endif
}

// 调度器语境：切出本协程（回到调度主协程）
void Fiber::swapOut() {
    Fiber* sched = Scheduler::GetMainFiber();
    SetThis(sched);
#ifdef SYLAR_HAS_ASAN
    // 当前运行在本协程上：保存本协程假栈，目标为调度协程
    __sanitizer_start_switch_fiber(&m_fake_stack, sched->getStackBottom(), sched->getStackSize());
#endif
    if (swapcontext(&m_ctx, &sched->m_ctx)) {
        throw std::logic_error("swapcontext error");
    }
#ifdef SYLAR_HAS_ASAN
    const void* ob = nullptr;
    size_t os = 0;
    __sanitizer_finish_switch_fiber(m_fake_stack, &ob, &os);
#endif
}

// 无调度器语境：切入本协程（从线程主协程）
void Fiber::call() {
    SetThis(this);
    m_state = EXEC;
#ifdef SYLAR_HAS_ASAN
    // 当前运行在线程主协程上：保存其假栈，目标为当前协程
    __sanitizer_start_switch_fiber(&t_threadFiber->m_fake_stack, getStackBottom(), getStackSize());
#endif
    if (swapcontext(&t_threadFiber->m_ctx, &m_ctx)) {
        throw std::logic_error("swapcontext error");
    }
#ifdef SYLAR_HAS_ASAN
    const void* ob = nullptr;
    size_t os = 0;
    __sanitizer_finish_switch_fiber(t_threadFiber->m_fake_stack, &ob, &os);
#endif
}

// 无调度器语境：切回线程主协程
void Fiber::back() {
    SetThis(t_threadFiber.get());
#ifdef SYLAR_HAS_ASAN
    // 当前运行在本协程上：保存本协程假栈，目标为线程主协程
    __sanitizer_start_switch_fiber(&m_fake_stack, t_threadFiber->getStackBottom(), t_threadFiber->getStackSize());
#endif
    if (swapcontext(&m_ctx, &t_threadFiber->m_ctx)) {
        throw std::logic_error("swapcontext error");
    }
#ifdef SYLAR_HAS_ASAN
    const void* ob = nullptr;
    size_t os = 0;
    __sanitizer_finish_switch_fiber(m_fake_stack, &ob, &os);
#endif
}

void Fiber::SetThis(Fiber* f) {
    t_fiber = f;
}

Fiber::ptr Fiber::GetThis() {
    if (t_fiber) {
        return t_fiber->shared_from_this();
    }
    Fiber::ptr main_fiber(new Fiber);
    SYLAR_ASSERT(t_fiber == main_fiber.get());
    t_threadFiber = main_fiber;
    return t_fiber->shared_from_this();
}

void Fiber::YieldToReady() {
    Fiber::ptr cur = GetThis();
    SYLAR_ASSERT(cur->m_state == EXEC);
    cur->m_state = READY;
    cur->swapOut();
}

void Fiber::YieldToHold() {
    Fiber::ptr cur = GetThis();
    SYLAR_ASSERT(cur->m_state == EXEC);
    cur->swapOut();
}

uint64_t Fiber::TotalFibers() {
    return s_fiber_count;
}

void Fiber::MainFunc() {
    Fiber::ptr cur = GetThis();
    SYLAR_ASSERT(cur);
#ifdef SYLAR_HAS_ASAN
    // 首次进入本协程：清除切换标记，初始化本协程的假栈视图
    const void* ob = nullptr;
    size_t os = 0;
    __sanitizer_finish_switch_fiber(cur->m_fake_stack, &ob, &os);
#endif
    try {
        cur->m_cb();
        cur->m_cb = nullptr;
        cur->m_state = TERM;
    } catch (std::exception& ex) {
        cur->m_state = EXCEPT;
        LOG_ERROR() << "Fiber Except: " << ex.what() << " fiber_id=" << cur->getId();
    } catch (...) {
        cur->m_state = EXCEPT;
        LOG_ERROR() << "Fiber Except: unknown, fiber_id=" << cur->getId();
    }

    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->swapOut();
    SYLAR_ASSERT2(false, "never reach fiber_id=" + std::to_string(raw_ptr->getId()));
}

void Fiber::CallerMainFunc() {
    Fiber::ptr cur = GetThis();
    SYLAR_ASSERT(cur);
#ifdef SYLAR_HAS_ASAN
    // 首次进入本协程：清除切换标记，初始化本协程的假栈视图
    const void* ob = nullptr;
    size_t os = 0;
    __sanitizer_finish_switch_fiber(cur->m_fake_stack, &ob, &os);
#endif
    try {
        cur->m_cb();
        cur->m_cb = nullptr;
        cur->m_state = TERM;
    } catch (std::exception& ex) {
        cur->m_state = EXCEPT;
        LOG_ERROR() << "Fiber Except: " << ex.what() << " fiber_id=" << cur->getId();
    } catch (...) {
        cur->m_state = EXCEPT;
        LOG_ERROR() << "Fiber Except: unknown, fiber_id=" << cur->getId();
    }

    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->back();
    SYLAR_ASSERT2(false, "never reach fiber_id=" + std::to_string(raw_ptr->getId()));
}

} // namespace sylar