#pragma once

namespace sylar {

// 不可拷贝 / 不可赋值基类
class Noncopyable {
public:
    Noncopyable() = default;
    ~Noncopyable() = default;
    Noncopyable(const Noncopyable&) = delete;
    Noncopyable& operator=(const Noncopyable&) = delete;
};

} // namespace sylar