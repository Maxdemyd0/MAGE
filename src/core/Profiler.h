#pragma once
// MAGE v1 - lightweight CPU profiling timers.
//
//   MAGE_PROFILE_SCOPE(engine.GetProfiler(), "Physics.Step");
//
// Scope names must be string literals (they are used as map keys by view).

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mage {

class Profiler {
public:
    struct Sample {
        std::string name;
        double lastMs = 0.0;
        double avgMs = 0.0;
        double maxMs = 0.0;
    };

    void Record(std::string_view name, double ms) {
        auto it = m_index.find(name);
        if (it == m_index.end()) {
            m_index.emplace(name, m_samples.size());
            m_samples.push_back(Sample{std::string(name), ms, ms, ms});
            return;
        }
        Sample& s = m_samples[it->second];
        s.lastMs = ms;
        s.avgMs = s.avgMs * 0.95 + ms * 0.05;
        s.maxMs = std::max(s.maxMs * 0.995, ms);
    }

    const Sample* Find(std::string_view name) const {
        auto it = m_index.find(name);
        return it == m_index.end() ? nullptr : &m_samples[it->second];
    }

    const std::vector<Sample>& Samples() const { return m_samples; }

private:
    std::unordered_map<std::string_view, size_t> m_index;
    std::vector<Sample> m_samples;
};

class ScopedTimer {
public:
    using Clock = std::chrono::steady_clock;
    ScopedTimer(Profiler& profiler, std::string_view name) : m_profiler(profiler), m_name(name), m_start(Clock::now()) {}
    ~ScopedTimer() {
        m_profiler.Record(m_name, std::chrono::duration<double, std::milli>(Clock::now() - m_start).count());
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Profiler& m_profiler;
    std::string_view m_name;
    Clock::time_point m_start;
};

} // namespace mage

#define MAGE_CONCAT_INNER(a, b) a##b
#define MAGE_CONCAT(a, b) MAGE_CONCAT_INNER(a, b)
#define MAGE_PROFILE_SCOPE(profiler, name) ::mage::ScopedTimer MAGE_CONCAT(_mageScopeTimer, __LINE__)(profiler, name)
