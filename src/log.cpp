#include "log.h"
#include <iostream>

namespace sylar {

std::atomic<int>& GlobalLogLevel() {
    static std::atomic<int> s_level(INFO);
    return s_level;
}

static const char* LevelStr(LogLevel lv) {
    switch (lv) {
        case DEBUG: return "DEBUG";
        case INFO:  return "INFO";
        case WARN:  return "WARN";
        case ERROR: return "ERROR";
        case FATAL: return "FATAL";
    }
    return "UNKNOWN";
}

LogLine::LogLine(const char* file, int line, LogLevel level)
    : m_file(file), m_line(line), m_level(level) {
    m_ss << "[" << LevelStr(m_level) << "]"
         << "[" << m_file << ":" << m_line << "] ";
}

LogLine::~LogLine() {
    if ((int)m_level < GlobalLogLevel().load(std::memory_order_relaxed)) {
        return;
    }
    m_ss << "\n";
    std::cout << m_ss.str();
    std::cout.flush();
}

} // namespace sylar