// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - thread-safe logging with an in-memory ring buffer (for the editor console).

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace mage {

enum class LogLevel : uint8_t { Trace, Info, Warn, Error };

struct LogEntry {
    LogLevel level = LogLevel::Info;
    double timeSeconds = 0.0;
    std::string message;
};

namespace Log {

void Write(LogLevel level, std::string_view message);

// Snapshot of the most recent lines (oldest first). Thread-safe.
std::vector<LogEntry> Snapshot(size_t maxEntries = 512);
// Monotonic counter that increases whenever a line is written.
uint64_t Revision();
void Clear();

template <class... Args>
void Trace(std::format_string<Args...> fmt, Args&&... args) { Write(LogLevel::Trace, std::format(fmt, std::forward<Args>(args)...)); }
template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) { Write(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...)); }
template <class... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) { Write(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...)); }
template <class... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) { Write(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...)); }

} // namespace Log
} // namespace mage
