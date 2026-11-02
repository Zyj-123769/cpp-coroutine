#include "util.h"
#include "fiber.h"
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace sylar {

pid_t GetThreadId() {
    return syscall(SYS_gettid);
}

uint64_t GetFiberId() {
    return Fiber::GetFiberId();
}

uint64_t GetCurrentMS() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000ull + tv.tv_usec / 1000;
}

uint64_t GetCurrentUS() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000ull * 1000ull + tv.tv_usec;
}

uint64_t GetMonotonicMS() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
}

uint64_t GetMonotonicUS() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000ull;
}

} // namespace sylar