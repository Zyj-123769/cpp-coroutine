#pragma once

#include <cstdint>

namespace sylar {

// 线程安全单例模板
template<class T>
class Singleton {
public:
    static T* GetInstance() {
        static T instance;
        return &instance;
    }
};

} // namespace sylar