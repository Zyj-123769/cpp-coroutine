#pragma once

#include <pthread.h>
#include <semaphore.h>
#include <memory>
#include <cstdint>
#include "noncopyable.h"

namespace sylar {

// 信号量（用于线程启动同步）
class Semaphore : Noncopyable {
public:
    Semaphore(uint32_t count = 0);
    ~Semaphore();
    void wait();
    void notify();
private:
    sem_t m_semaphore;
};

// 通用局部锁模板（加锁/解锁 RAII）
template<class T>
struct ScopedLockImpl {
    ScopedLockImpl(T& mutex) : m_mutex(mutex) { m_mutex.lock(); m_locked = true; }
    ~ScopedLockImpl() { unlock(); }
    void lock()   { if (!m_locked) { m_mutex.lock(); m_locked = true; } }
    void unlock() { if (m_locked)  { m_mutex.unlock(); m_locked = false; } }
private:
    T& m_mutex;
    bool m_locked;
};

// 局部读锁模板
template<class T>
struct ReadScopedLockImpl {
    ReadScopedLockImpl(T& mutex) : m_mutex(mutex) { m_mutex.rdlock(); m_locked = true; }
    ~ReadScopedLockImpl() { unlock(); }
    void lock()   { if (!m_locked) { m_mutex.rdlock(); m_locked = true; } }
    void unlock() { if (m_locked)  { m_mutex.unlock(); m_locked = false; } }
private:
    T& m_mutex;
    bool m_locked;
};

// 局部写锁模板
template<class T>
struct WriteScopedLockImpl {
    WriteScopedLockImpl(T& mutex) : m_mutex(mutex) { m_mutex.wrlock(); m_locked = true; }
    ~WriteScopedLockImpl() { unlock(); }
    void lock()   { if (!m_locked) { m_mutex.wrlock(); m_locked = true; } }
    void unlock() { if (m_locked)  { m_mutex.unlock(); m_locked = false; } }
private:
    T& m_mutex;
    bool m_locked;
};

// 互斥锁（pthread 封装）
class Mutex : Noncopyable {
public:
    typedef ScopedLockImpl<Mutex> Lock;
    Mutex()  { pthread_mutex_init(&m_mutex, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&m_mutex); }
    void lock()   { pthread_mutex_lock(&m_mutex); }
    void unlock() { pthread_mutex_unlock(&m_mutex); }
private:
    pthread_mutex_t m_mutex;
};

// 自旋锁
class Spinlock : Noncopyable {
public:
    typedef ScopedLockImpl<Spinlock> Lock;
    Spinlock()  { pthread_spin_init(&m_mutex, 0); }
    ~Spinlock() { pthread_spin_destroy(&m_mutex); }
    void lock()   { pthread_spin_lock(&m_mutex); }
    void unlock() { pthread_spin_unlock(&m_mutex); }
private:
    pthread_spinlock_t m_mutex;
};

// 读写锁
class RWMutex : Noncopyable {
public:
    typedef ReadScopedLockImpl<RWMutex>  ReadLock;
    typedef WriteScopedLockImpl<RWMutex> WriteLock;
    RWMutex()  { pthread_rwlock_init(&m_lock, nullptr); }
    ~RWMutex() { pthread_rwlock_destroy(&m_lock); }
    void rdlock() { pthread_rwlock_rdlock(&m_lock); }
    void wrlock() { pthread_rwlock_wrlock(&m_lock); }
    void unlock() { pthread_rwlock_unlock(&m_lock); }
private:
    pthread_rwlock_t m_lock;
};

} // namespace sylar