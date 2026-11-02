#pragma once

#include <sstream>
#include <string>
#include <atomic>

namespace sylar {

// 日志级别（数值越大越严重）
enum LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    FATAL = 4,
};

// 全局日志级别（默认 INFO）
std::atomic<int>& GlobalLogLevel();

// 单行日志辅助类：在析构时输出，支持流式写法 LOG_INFO() << "..."
class LogLine {
public:
    LogLine(const char* file, int line, LogLevel level);
    ~LogLine();
    std::ostringstream& stream() { return m_ss; }
private:
    std::ostringstream m_ss;
    const char* m_file;
    int m_line;
    LogLevel m_level;
};

} // namespace sylar

#define SCOPE_LOG(level) sylar::LogLine(__FILE__, __LINE__, sylar::level).stream()
#define LOG_DEBUG() SCOPE_LOG(DEBUG)
#define LOG_INFO()  SCOPE_LOG(INFO)
#define LOG_WARN()  SCOPE_LOG(WARN)
#define LOG_ERROR() SCOPE_LOG(ERROR)
#define LOG_FATAL() SCOPE_LOG(FATAL)