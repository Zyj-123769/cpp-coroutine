#pragma once

#include <pthread.h>
#include <functional>
#include <memory>
#include <string>
#include "mutex.h"
#include "noncopyable.h"

namespace sylar {

// 线程封装（pthread + 信号量同步启动）
class Thread : Noncopyable {
public:
    typedef std::shared_ptr<Thread> ptr;

    Thread(std::function<void()> cb, const std::string& name = "");
    // 析构语义：未 join 的对象在析构时 pthread_detach，因此**线程可能在本对象销毁后
    // 仍在运行**。这样做是安全的：run() 在通知构造函数放行之前就把 m_cb swap 到了
    // 自己的局部变量，此后线程只访问自己的栈与 thread_local，不再触碰 Thread 成员。
    // 换句话说，析构后仍有「活着的线程」是允许的，但不再有「指向本对象的访问」。
    ~Thread();

    pid_t getId() const { return m_id; }
    const std::string& getName() const { return m_name; }

    void join();

    static Thread* GetThis();
    static const std::string& GetName();
    static void SetName(const std::string& name);

private:
    static void* run(void* arg);

private:
    pid_t m_id = -1;
    pthread_t m_thread = 0;
    std::function<void()> m_cb;
    std::string m_name;
    Semaphore m_semaphore;
};

} // namespace sylar