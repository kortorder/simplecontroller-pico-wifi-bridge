// Original SCX2 to XInput report adapter. Protocol reference: GP2040-CE's
// MIT-licensed XInputDescriptors.h / XInputDriver.cpp (see docs/CRONUS_ZEN.md).
#pragma once
#include "scx2.hpp"

namespace scx2 {
using XInputReport = std::array<std::uint8_t,20>;
inline std::optional<XInputReport> xinput_report(const State& state, bool external_right_stick=false) {
    if (!state.valid()) return std::nullopt;
    XInputReport out{}; out[1]=20;
    // SCX2 order: A B X Y LB RB Back Start LS RS.
    constexpr std::uint16_t masks[] = {0x1000,0x2000,0x4000,0x8000,0x0100,0x0200,0x0020,0x0010,0x0040,0x0080};
    constexpr std::uint8_t hats[] = {1,9,8,10,2,6,4,5,0};
    std::uint16_t buttons=hats[state.hat];
    for(unsigned i=0;i<10;++i) if(state.buttons & (1u<<i)) buttons|=masks[i];
    out[2]=std::uint8_t(buttons); out[3]=std::uint8_t(buttons>>8);
    // Round normalized 16-bit pressure to the nearest 8-bit trigger step.
    out[4]=std::uint8_t((std::uint32_t(state.triggers.left)*255+32767)/65535);
    out[5]=std::uint8_t((std::uint32_t(state.triggers.right)*255+32767)/65535);
    for(unsigned i=0;i<4;++i) {
        // SCX2 and XInput both use up-positive Y. No XAC port reordering.
        const auto value=std::uint16_t(external_right_stick && i>=2 ? 0 : state.axes[i]);
        out[6+i*2]=std::uint8_t(value); out[7+i*2]=std::uint8_t(value>>8);
    }
    return out;
}
}
