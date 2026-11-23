// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "core/Log.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

namespace mage::Log {
namespace {
constexpr size_t kMaxEntries = 4096;

struct LogState {
    std::mutex mutex;
    std::deque<LogEntry> entries;
    uint64_t revision = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

LogState& State() {
    static LogState s;
    return s;
}

const char* LevelTag(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}
} // namespace

void Write(LogLevel level, std::string_view message) {
    LogState& s = State();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();

    std::lock_guard lock(s.mutex);
    std::FILE* stream = level >= LogLevel::Warn ? stderr : stdout;
    std::fprintf(stream, "[%9.3f] [%s] %.*s\n", t, LevelTag(level), static_cast<int>(message.size()), message.data());
    std::fflush(stream); // keep logs intact if the process crashes

    s.entries.push_back(LogEntry{level, t, std::string(message)});
    if (s.entries.size() > kMaxEntries) s.entries.pop_front();
    ++s.revision;
}

std::vector<LogEntry> Snapshot(size_t maxEntries) {
    LogState& s = State();
    std::lock_guard lock(s.mutex);
    const size_t count = std::min(maxEntries, s.entries.size());
    return std::vector<LogEntry>(s.entries.end() - static_cast<std::ptrdiff_t>(count), s.entries.end());
}

uint64_t Revision() {
    LogState& s = State();
    std::lock_guard lock(s.mutex);
    return s.revision;
}

void Clear() {
    LogState& s = State();
    std::lock_guard lock(s.mutex);
    s.entries.clear();
    ++s.revision;
}

} // namespace mage::Log
