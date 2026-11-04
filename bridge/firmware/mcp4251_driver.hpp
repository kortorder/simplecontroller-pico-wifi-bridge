#pragma once
#include "trigger_output.hpp"
#include <array>
#include <cstdint>
#include <limits>

namespace scx2 {
// The circuit is specified in docs/ANALOG_TRIGGER_HARDWARE.md. This interface
// permits deterministic checks without pretending to measure a real jack.
class Mcp4251IO {
public:
    virtual ~Mcp4251IO() = default;
    virtual bool power_good() const noexcept = 0;
    virtual std::uint64_t monotonic_us() const noexcept = 0;
    virtual void gate(bool enabled) noexcept = 0;
    // Exactly one CS-framed, bounded, mode-0 16-bit transaction.
    virtual bool transfer(std::array<std::uint8_t, 2> tx,
                          std::array<std::uint8_t, 2>& rx) noexcept = 0;
};

class Mcp4251Triggers final : public TriggerOutputBackend {
public:
    explicit Mcp4251Triggers(Mcp4251IO& io) : io_(io) {}
    bool initialize() noexcept {
        release(); healthy_ = false;
        if (!io_.power_good() || write(4, 0x1ff)!=TriggerApplyResult::accepted ||
            verify(4, 0x1ff)!=TriggerApplyResult::accepted || set_wipers(0, 0)!=TriggerApplyResult::accepted) return fail();
        healthy_ = true;
        return true;
    }
    bool ready() const noexcept override { return healthy_ && io_.power_good(); }
    bool apply(TriggerPressure pressure) noexcept override {
        return apply_before(pressure,unbounded)==TriggerApplyResult::accepted;
    }
    TriggerApplyResult apply_before(TriggerPressure pressure,std::uint64_t deadline) noexcept override {
        // Open first for a complete release, even if SPI has subsequently failed.
        if (pressure.neutral()) release();
        if (!ready()) return failed_result();
        if (!before(deadline)) return expired_result();
        const auto left = wiper(pressure.left), right = wiper(pressure.right);
        // Readback catches missing wiring, a reset-to-midscale and command errors.
        // Unchanged heartbeats never cycle the relay or interrupt a held trigger.
        auto operation=verify(4, 0x1ff, deadline);
        if (operation!=TriggerApplyResult::accepted) return operation_failed(operation);
        if (!enabled_ || left != left_ || right != right_) {
            operation=set_wipers(left, right, deadline);
        } else {
            operation=verify(0, left, deadline);
            if (operation==TriggerApplyResult::accepted) operation=verify(1, right, deadline);
        }
        if (operation!=TriggerApplyResult::accepted) return operation_failed(operation);
        if (!ready()) return failed_result();
        // SPI can finish after the lease time supplied at USB completion. Never
        // enable from that stale pre-SPI timestamp; ordinary expiry is recoverable.
        if (!before(deadline)) return expired_result();
        left_ = left; right_ = right;
        enabled_ = !pressure.neutral();
        io_.gate(enabled_);
        return TriggerApplyResult::accepted;
    }
    void release() noexcept override { io_.gate(false); enabled_ = false; }
    static constexpr std::uint16_t wiper(std::uint16_t pressure) noexcept {
        return std::uint16_t((std::uint32_t(pressure) * 256u + 32767u) / 65535u);
    }
private:
    static constexpr auto unbounded=std::numeric_limits<std::uint64_t>::max();
    Mcp4251IO& io_;
    bool healthy_ = false, enabled_ = false;
    std::uint16_t left_ = 0, right_ = 0;
    bool fail() noexcept { release(); healthy_ = false; return false; }
    bool before(std::uint64_t deadline) const noexcept { return deadline==unbounded || io_.monotonic_us()<deadline; }
    TriggerApplyResult failed_result() noexcept { (void)fail(); return TriggerApplyResult::fault; }
    TriggerApplyResult expired_result() noexcept { release(); return TriggerApplyResult::expired; }
    TriggerApplyResult operation_failed(TriggerApplyResult result) noexcept {
        // Preserve the cause: a real bus/readback failure stays a hardware fault
        // even if its elapsed transaction time also crossed the lease deadline.
        return result==TriggerApplyResult::expired && ready() ? expired_result() : failed_result();
    }
    TriggerApplyResult write(std::uint8_t address, std::uint16_t value,std::uint64_t deadline=unbounded) noexcept {
        if (!before(deadline)) return TriggerApplyResult::expired;
        std::array<std::uint8_t, 2> rx{};
        return io_.transfer({std::uint8_t((address << 4) | ((value >> 8) & 1)), std::uint8_t(value)}, rx) && (rx[0] & 2) ?
            TriggerApplyResult::accepted : TriggerApplyResult::fault;
    }
    TriggerApplyResult verify(std::uint8_t address, std::uint16_t expected,std::uint64_t deadline=unbounded) noexcept {
        if (!before(deadline)) return TriggerApplyResult::expired;
        std::array<std::uint8_t, 2> rx{};
        return io_.transfer({std::uint8_t((address << 4) | 0x0c), 0xff}, rx) && (rx[0] & 2) &&
            std::uint16_t((std::uint16_t(rx[0] & 1) << 8) | rx[1]) == expected ?
            TriggerApplyResult::accepted : TriggerApplyResult::fault;
    }
    TriggerApplyResult set_wipers(std::uint16_t left, std::uint16_t right,std::uint64_t deadline=unbounded) noexcept {
        auto result=write(0, left, deadline);
        if (result==TriggerApplyResult::accepted) result=write(1, right, deadline);
        if (result==TriggerApplyResult::accepted) result=verify(0, left, deadline);
        if (result==TriggerApplyResult::accepted) result=verify(1, right, deadline);
        return result;
    }
};
} // namespace scx2
