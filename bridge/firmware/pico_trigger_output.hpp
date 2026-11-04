#pragma once
#include "mcp4251_driver.hpp"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include <atomic>

// Fixed pin assignment for the documented MCP4251 + supervised PhotoMOS circuit.
// No pin is initialized unless SC_TRIGGER_MCP4251 is explicitly enabled.
class PicoTriggerIO final : public scx2::Mcp4251IO {
public:
    static constexpr unsigned miso = 16, cs = 17, sck = 18, mosi = 19, enable = 20, good = 21;
    void initialize() {
        gpio_init(enable); gpio_put(enable, false); gpio_set_dir(enable, GPIO_OUT);
        gpio_init(good); gpio_set_dir(good, GPIO_IN); gpio_pull_down(good);
        // Boot may begin with POWER_GOOD low during the supervisor delay.
        // Subsequent falling edges must survive recovery between main-loop polls.
        gpio_set_irq_enabled(good, GPIO_IRQ_EDGE_FALL, false);
        gpio_acknowledge_irq(good, GPIO_IRQ_EDGE_FALL);
        power_fault_.store(false, std::memory_order_relaxed);
        instance_ = this;
        gpio_set_irq_enabled_with_callback(good, GPIO_IRQ_EDGE_FALL, true, power_edge);
        gpio_init(cs); gpio_put(cs, true); gpio_set_dir(cs, GPIO_OUT);
        spi_init(spi0, 100'000);
        spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
        for (const auto pin : {miso, sck, mosi}) gpio_set_function(pin, GPIO_FUNC_SPI);
        gpio_pull_down(miso); // An absent chip must not pass zero-register checks.
        // MCP120 can hold reset for 700 ms. No network session exists yet.
        sleep_ms(1000);
    }
    bool power_good() const noexcept override {
        return !power_fault_.load(std::memory_order_acquire) && gpio_get(good) &&
            !(gpio_get_irq_event_mask(good) & GPIO_IRQ_EDGE_FALL);
    }
    std::uint64_t monotonic_us() const noexcept override { return time_us_64(); }
    void gate(bool active) noexcept override {
        // Bound only this check/write, not SPI. An ISR between a positive check
        // and gpio_put(true) must never overwrite its own emergency gate-off.
        const auto saved = save_and_disable_interrupts();
        if (!gpio_get(good) || (gpio_get_irq_event_mask(good) & GPIO_IRQ_EDGE_FALL))
            power_fault_.store(true, std::memory_order_release);
        gpio_put(enable, active && power_good());
        restore_interrupts(saved);
    }
    bool transfer(std::array<std::uint8_t, 2> tx, std::array<std::uint8_t, 2>& rx) noexcept override {
        if (!power_good()) return false;
        const auto deadline = time_us_64() + 2'000;
        gpio_put(cs, false); busy_wait_us_32(1);
        for (unsigned i = 0; i < 2; ++i) {
            while (!spi_is_writable(spi0)) {
                if (time_us_64() >= deadline || !power_good()) { gpio_put(cs, true); return false; }
            }
            spi_get_hw(spi0)->dr = tx[i];
            while (!spi_is_readable(spi0)) {
                if (time_us_64() >= deadline || !power_good()) { gpio_put(cs, true); return false; }
            }
            rx[i] = std::uint8_t(spi_get_hw(spi0)->dr);
        }
        while (spi_is_busy(spi0)) {
            if (time_us_64() >= deadline || !power_good()) { gpio_put(cs, true); return false; }
        }
        busy_wait_us_32(1); gpio_put(cs, true);
        return power_good();
    }
private:
    inline static PicoTriggerIO* instance_ = nullptr; // One firmware adapter, lives throughout main.
    std::atomic<bool> power_fault_{false};
    static void power_edge(unsigned pin, std::uint32_t events) {
        if (pin == good && (events & GPIO_IRQ_EDGE_FALL) && instance_) {
            instance_->power_fault_.store(true, std::memory_order_release);
            gpio_put(enable, false); // No SPI, allocation, Receiver or network work in the ISR.
        }
    }
};
