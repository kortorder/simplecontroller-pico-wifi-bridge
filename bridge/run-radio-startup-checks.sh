#!/bin/bash
set -euo pipefail
SC_RADIO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SC_RADIO_WORK="$(mktemp -d "${TMPDIR:-/tmp}"/simplecontroller-radio-checks-XXXXXX)"
# This one host-only translation unit supplies the small SDK surface that the
# real main loop calls. No hardware library or user's private header is linked.
for header in pico/stdlib.h pico/cyw43_arch.h pico/rand.h hardware/watchdog.h tusb.h lwip/udp.h lwip/pbuf.h bridge_config.h; do
  mkdir -p "$SC_RADIO_WORK/shim/$(dirname "$header")"
  touch "$SC_RADIO_WORK/shim/$header"
done
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -I "$SC_RADIO_ROOT/include" -I "$SC_RADIO_WORK/shim" \
  "$SC_RADIO_ROOT/tests/radio_usb_startup_checks.cpp" "$SC_RADIO_ROOT/src/scx2.cpp" -o "$SC_RADIO_WORK/checks"
"$SC_RADIO_WORK/checks" live
"$SC_RADIO_WORK/checks" retired
