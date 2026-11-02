#pragma once

#include <sstream>
#include <stdexcept>
#include <string>

#define SYLAR_LIKELY(x)   __builtin_expect(!!(x), 1)
#define SYLAR_UNLIKELY(x) __builtin_expect(!!(x), 0)

// 断言：失败时抛出异常，便于在测试中定位问题
#define SYLAR_ASSERT(x) \
    do { \
        if (!(x)) { \
            throw std::logic_error(std::string("assertion failed: ") + #x \
                + " @ " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)

// 断言（带附加信息 msg，支持 << 流式写法）
#define SYLAR_ASSERT2(x, msg) \
    do { \
        if (!(x)) { \
            std::ostringstream _sylar_assert_oss; \
            _sylar_assert_oss << msg; \
            throw std::logic_error(std::string("assertion failed: ") + #x \
                + " (" + _sylar_assert_oss.str() + ")" \
                + " @ " + __FILE__ + ":" + std::to_string(__LINE__)); \
        } \
    } while (0)
