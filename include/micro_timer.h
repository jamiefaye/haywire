#pragma once

#include <chrono>
#include <string>
#include <iostream>
#include <cmath>

namespace Haywire {

// Lightweight micro-benchmarking timer with min/avg/max stats
class MicroTimer {
public:
    MicroTimer(const std::string& name, int reportEveryN = 1000, bool resetAfterReport = false)
        : name_(name), reportInterval_(reportEveryN), resetAfterReport_(resetAfterReport),
          count_(0), totalNs_(0), minNs_(1e18), maxNs_(0) {}

    // Start timing an operation
    void Start() {
        start_ = std::chrono::high_resolution_clock::now();
    }

    // Stop timing and record the measurement
    void Stop() {
        auto end = std::chrono::high_resolution_clock::now();
        int64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();

        count_++;
        totalNs_ += elapsed;
        if (elapsed < minNs_) minNs_ = elapsed;
        if (elapsed > maxNs_) maxNs_ = elapsed;

        // Report stats periodically
        if (count_ % reportInterval_ == 0) {
            Report();
            if (resetAfterReport_) {
                Reset();
            }
        }
    }

    // Force report current stats
    void Report() {
        if (count_ == 0) return;

        int64_t avgNs = totalNs_ / count_;
        std::cout << "[TIMER] " << name_
                  << " (n=" << count_ << "): "
                  << "avg=" << FormatTime(avgNs) << " "
                  << "min=" << FormatTime(minNs_) << " "
                  << "max=" << FormatTime(maxNs_) << "\n";
    }

    // Reset stats
    void Reset() {
        count_ = 0;
        totalNs_ = 0;
        minNs_ = 1e18;
        maxNs_ = 0;
    }

private:
    std::string name_;
    int reportInterval_;
    bool resetAfterReport_;
    int count_;
    int64_t totalNs_;
    int64_t minNs_;
    int64_t maxNs_;
    std::chrono::high_resolution_clock::time_point start_;

    // Format time with appropriate units
    static std::string FormatTime(int64_t ns) {
        if (ns < 1000) {
            return std::to_string(ns) + "ns";
        } else if (ns < 1000000) {
            return std::to_string(ns / 1000) + "us";
        } else {
            return std::to_string(ns / 1000000) + "ms";
        }
    }
};

// RAII wrapper for automatic start/stop
class ScopedTimer {
public:
    ScopedTimer(MicroTimer& timer) : timer_(timer) {
        timer_.Start();
    }
    ~ScopedTimer() {
        timer_.Stop();
    }
private:
    MicroTimer& timer_;
};

} // namespace Haywire

// Convenience macros for easy enable/disable
#define MICRO_TIMER_ENABLE 1  // Set to 1 to enable timing

#if MICRO_TIMER_ENABLE
    #define MICRO_TIMER_DECL(name, interval) static Haywire::MicroTimer name(#name, interval, true)
    #define MICRO_TIMER_START(timer) timer.Start()
    #define MICRO_TIMER_STOP(timer) timer.Stop()
    #define MICRO_TIMER_SCOPE(timer) Haywire::ScopedTimer _scoped_##timer(timer)
#else
    #define MICRO_TIMER_DECL(name, interval)
    #define MICRO_TIMER_START(timer)
    #define MICRO_TIMER_STOP(timer)
    #define MICRO_TIMER_SCOPE(timer)
#endif
