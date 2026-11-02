# C++ 协程库（基于 ucontext 的用户态有栈协程）  [![CI](https://github.com/Zyj-123769/cpp-coroutine/actions/workflows/ci.yml/badge.svg)](https://github.com/Zyj-123769/cpp-coroutine/actions/workflows/ci.yml)

> A stackful coroutine library in C++ built on `ucontext`, with an epoll-driven IO scheduler, `dlsym` syscall hook, and a min-heap timer — so you can write **synchronous-looking code that runs async**.

一个学习/练手向的 C++ 协程「轮子」项目：用**主协程 + 调度协程 + 子协程**三层切换模型，结合 **epoll IO 调度 + hook + 最小堆定时器**，把异步网络编程写得像同步代码一样直白。

- 语言：C++11/14（Linux only）
- 核心机制：`ucontext`（makecontext/swapcontext）、pthread、epoll、`dlsym` 地址重写、最小堆定时器
- 对标：sylar 协程框架、腾讯 libco

---

## ✨ 特性

- **有栈协程（stackful coroutine）**：每协程独立 `mmap` 栈，低地址 `PROT_NONE` 保护页让栈溢出直接 SIGSEGV 而非静默踩内存。
- **三层上下文切换**：每线程用 TLS 保存主协程 / 调度协程 / 当前子协程，协作式调度（`m_runInScheduler` 标记区分切换目标）。
- **IO 调度器（IOScheduler）**：协程等待 IO 时让出 CPU，fd 就绪后自动唤醒放回队列；1000 个挂起协程 CPU 占用接近 0（无忙等）。
- **系统调用 Hook**：通过 `dlsym(RTLD_NEXT, ...)` 拦截 `read/write/connect/sleep/usleep/nanosleep` 等，把阻塞调用改造成「挂起协程 + IO/定时器唤醒」，业务代码写同步风格即可扛高并发。
- **定时器（CLOCK_MONOTONIC）**：最小堆按到期时间排序，堆顶到期时间驱动 `epoll_wait` 超时；基于单调时钟构造，**免疫系统时间回拨/NTP 步进**。
- **可观测性**：内置日志模块、每线程 ID / 协程 ID 工具；协程栈切换处接入 ASan 的 `start/finish_switch_fiber` 注解，让 AddressSanitizer 正确识别协程栈边界。
- **构建与质量保障**：构建系统正确禁用 `_FORTIFY_SOURCE` 并注释原因；提供 UBSan 与 ThreadSanitizer 目标；CI 在 g++/clang 双编译器下跑 UBSan 全量测试。

---

## 📁 目录结构

```
cpp-coroutine/
├── include/            # 头文件
│   ├── fiber.h          # 协程类（ucontext + 独立栈）
│   ├── scheduler.h      # 协作式调度器（就绪队列 + 三类上下文 TLS）
│   ├── iomanager.h      # epoll 事件驱动的 IO 调度器
│   ├── timer.h          # 最小堆定时器（CLOCK_MONOTONIC）
│   ├── hook.h           # dlsym 系统调用拦截
│   ├── fd_manager.h     # fd 上下文（事件/超时/协程映射）
│   ├── mutex.h          # 协程/线程锁与信号量
│   ├── thread.h         # pthread 线程封装
│   ├── log.h            # 日志
│   ├── util.h           # 时间/ID 工具（含单调时钟接口）
│   └── ...              # macro / noncopyable / singleton
├── src/                # 实现（.cpp）
├── tests/              # 单元测试（test_fiber / test_scheduler / test_ioscheduler
│                       #   / test_io_concurrency / test_timer / test_hook
│                       #   / test_fiber_exception / test_stack_overflow）
├── examples/           # echo_server.cpp（协程版 echo server）
├── bench/              # http_server.cpp + native_http_server.cpp + bench.sh（ab 压测）
├── Makefile            # make / make check(UBSan) / make check-tsan(TSan, 实验)
├── CMakeLists.txt      # 备选构建
├── Dockerfile          # 容器内 make + make check 一键验证
└── 代码规范与Review清单.md  # 设计说明、代码规范、Review 清单与评分
```

---

## 🔧 构建

需要 Linux + g++/clang（C++11 及以上）。

```bash
git clone git@github.com:Zyj-123769/cpp-coroutine.git
cd cpp-coroutine

make            # 常规编译：库对象 + 全部测试 + echo_server + 压测程序
make check      # UBSan 重建（-fsanitize=undefined -fno-sanitize-recover=all）并跑全量测试
make check-tsan # 实验性：ThreadSanitizer 重建 + 全量测试（详见下文「测试与 CI」）
```

> **关于 ASan**：本库未提供 ASan 构建目标——AddressSanitizer 会拦截 `read/write` 等 libc 调用，与协程 `dlsym` hook 冲突。UBSan 是 UB 检测的推荐选择。

### 容器化验证（无需本地 Linux）

若本机是 Windows/macOS，可用 Docker 一键在 `gcc:bookworm` 容器内完成编译 + UBSan 全量测试：

```bash
bash docker_verify.sh          # 等价于：docker build -t sylar-coroutine-verify .
```

---

## 🚀 快速上手

### 1. 最小的协程切换（Fiber）

```cpp
#include "fiber.h"
#include <cstdio>
using namespace sylar;

int main() {
    Fiber::GetThis();                                   // 建立当前线程主协程
    Fiber::ptr sub(new Fiber([] {
        printf("  SUB #1\n");
        Fiber::GetThis()->back();                       // 让出，切回主协程
        printf("  SUB #2\n");
    }, 0, /*use_caller=*/true));

    printf("MAIN #1\n");  sub->call();                  // MAIN -> SUB
    printf("MAIN #2\n");  sub->call();                  // 再次进入，SUB 结束后切回
    printf("MAIN #3\n");
    return 0;
}
// 输出：MAIN #1 -> SUB #1 -> MAIN #2 -> SUB #2 -> MAIN #3
```

### 2. IO 调度 + 定时器（IOScheduler）

```cpp
#include "iomanager.h"
#include <unistd.h>
using namespace sylar;

int main() {
    IOManager iom(1, true, "demo");     // 1 调度线程，在调用线程内运行
    iom.addTimer(1000, [] {             // 1 秒后触发（基于 CLOCK_MONOTONIC）
        printf("timer fired!\n");
    });
    iom.schedule([] {
        printf("task running in coroutine\n");
    });
    iom.stop();                          // 等待所有任务/IO/定时器完成
    return 0;
}
```

### 3. 同步风格写服务端（Hook）

见 [`examples/echo_server.cpp`](examples/echo_server.cpp)：业务代码直接写 `read(fd, ...)` / `write(fd, ...)` / `accept(...)`，配合 `hook.h` 被 IO 调度器接管为「协程级阻塞」，无需手动管理 epoll 回调。

---

## 🧪 测试与 CI

- **测试集**（`tests/`，由 `tests/run_tests.sh` 统一运行）：覆盖协程切换、调度器、IO 并发（1000 挂起协程 CPU 占用验证）、定时器精度、hook（usleep 实测时长）、异常隔离、栈溢出保护。
- **CI**：[`.github/workflows/ci.yml`](.github/workflows/ci.yml) 在每次 push / PR 于 GitHub 托管的 `ubuntu-latest` 上，用 **g++ 与 clang++ 双编译器矩阵** 跑 `make` + `make check`（UBSan 全量测试）。
- **ThreadSanitizer（实验）**：[`.github/workflows/tsan.yml`](.github/workflows/tsan.yml) 提供手动触发的 TSan 入口（`make check-tsan`）。默认未设为门禁，因为 TSan 需配合协程切换处的 `__tsan_switch_to_fiber` 注解才能正确识别 `swapcontext` 的栈切换；相关适配完成后可转正为常驻门禁。

---

## 📖 设计文档

完整的分阶段开发路线、模块拆解、核心机制讲解与验收清单见 **[`03_C++_协程库.md`](03_C++_协程库.md)**。
代码规范、Review 检查清单与评分见 **[`代码规范与Review清单.md`](代码规范与Review清单.md)**。

## 🔗 参考

| 类型 | 链接 | 说明 |
|---|---|---|
| 官方项目页 | https://programmercarl.com/other/project_coroutine.html | 协程调度 + epoll + hook + 定时器 定位 |
| 官方仓库 | https://github.com/youngyangyang04/coroutine-lib | thread/fiber/scheduler/ioscheduler/timer/hook 模块参考 |
| 公开资料 | https://github.com/Tencent/libco | 微信 libco（设计对标） |

## 📄 License

[MIT](LICENSE)
