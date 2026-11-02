#pragma once

#include <cstdint>
#include <sys/types.h>

namespace sylar {

// 获取当前线程 id（tid）
pid_t GetThreadId();

// 获取当前协程 id（无协程时返回 0）
uint64_t GetFiberId();

// 当前时间（毫秒 / 微秒，基于墙上时钟 / CLOCK_REALTIME）
// 适合需要"真实日历时间"的场景（如日志时间戳）。
uint64_t GetCurrentMS();
uint64_t GetCurrentUS();

// 单调时钟（毫秒 / 微秒，基于 CLOCK_MONOTONIC）
// 不受系统时间调整、NTP 步进、手动改时钟影响，只增不减。
// 适合定时器到期、IO 超时、耗时测量等"相对时间间隔"场景。
uint64_t GetMonotonicMS();
uint64_t GetMonotonicUS();

} // namespace sylar