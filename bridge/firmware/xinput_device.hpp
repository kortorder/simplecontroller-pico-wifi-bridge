#pragma once
#include <cstddef>
#include <cstdint>

bool sc_xinput_ready();
bool sc_xinput_send(const std::uint8_t* report, std::size_t length);
// Called only after the TinyUSB endpoint reports actual transfer completion.
extern "C" void sc_xinput_report_complete_cb(const std::uint8_t*, std::uint16_t);
extern "C" void sc_xinput_report_failed_cb();
extern "C" void sc_xinput_reset_cb();
