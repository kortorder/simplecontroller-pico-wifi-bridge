// Original, host-testable startup policy. No SDK/network dependency.
#pragma once
#include <cstdint>
#include <limits>
namespace scx2 {
class ListenerStartRetry {
public:
    static constexpr unsigned maximum_attempts = 5;
    static constexpr std::uint64_t delay_us = 1'000'000;
    bool due(std::uint64_t now) const { return !complete_ && !faulted_ && now >= next_; }
    void failed(std::uint64_t now) {
        if (complete_ || faulted_) return;
        ++failures_;
        if (failures_ >= maximum_attempts || now > std::numeric_limits<std::uint64_t>::max() - delay_us) {
            faulted_ = true; return;
        }
        next_ = now + delay_us;
    }
    void succeeded() { if (!faulted_) complete_ = true; }
    void reset() { *this = ListenerStartRetry{}; }
    bool faulted() const { return faulted_; }
    unsigned failures() const { return failures_; }
private:
    std::uint64_t next_ = 0;
    unsigned failures_ = 0;
    bool complete_ = false, faulted_ = false;
};
}
