#pragma once
#include "pico/stdlib.h"
inline std::uint32_t save_and_disable_interrupts() {
    auto prior=fake_pico::interrupt_enabled;fake_pico::interrupt_enabled=false;return prior;
}
inline void restore_interrupts(std::uint32_t prior) {fake_pico::interrupt_enabled=prior;fake_pico::dispatch();}
