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

clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -DSC_USB_XINPUT=1 -DSC_XINPUT_EXTERNAL_RS=1 \
  -I "$SC_RADIO_ROOT/include" -I "$SC_RADIO_WORK/shim" \
  "$SC_RADIO_ROOT/tests/radio_usb_startup_checks.cpp" "$SC_RADIO_ROOT/src/scx2.cpp" -o "$SC_RADIO_WORK/xinput-checks"
"$SC_RADIO_WORK/xinput-checks" live
"$SC_RADIO_WORK/xinput-checks" retired

clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -I "$SC_RADIO_ROOT/include" -I "$SC_RADIO_WORK/shim" \
  "$SC_RADIO_ROOT/tests/chair_relay_adapter_checks.cpp" "$SC_RADIO_ROOT/src/scx2.cpp" -o "$SC_RADIO_WORK/chair-relay-checks"
"$SC_RADIO_WORK/chair-relay-checks"
"$SC_RADIO_WORK/chair-relay-checks" pm-failed

clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -DSC_CHAIR_RELAY=1 -I "$SC_RADIO_ROOT/include" -I "$SC_RADIO_WORK/shim" \
  "$SC_RADIO_ROOT/tests/radio_usb_startup_checks.cpp" "$SC_RADIO_ROOT/src/scx2.cpp" -o "$SC_RADIO_WORK/relay-startup-checks"
"$SC_RADIO_WORK/relay-startup-checks" live
"$SC_RADIO_WORK/relay-startup-checks" pm-failed

# Check the actual production lwIP configuration in both compile variants.
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsyntax-only "$SC_RADIO_ROOT/tests/lwip_pool_checks.cpp"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -DSC_CHAIR_RELAY=1 -fsyntax-only "$SC_RADIO_ROOT/tests/lwip_pool_checks.cpp"

# Reproduce the original two-slot fault with the same finite-pool adapter test.
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
  -DSC_TEST_OLD_UDP_POOL=1 -I "$SC_RADIO_ROOT/include" -I "$SC_RADIO_WORK/shim" \
  "$SC_RADIO_ROOT/tests/chair_relay_adapter_checks.cpp" "$SC_RADIO_ROOT/src/scx2.cpp" -o "$SC_RADIO_WORK/old-pool-checks"
if "$SC_RADIO_WORK/old-pool-checks" > "$SC_RADIO_WORK/old-pool.log" 2>&1; then
  echo "Old two-slot pool unexpectedly passed the regression" >&2
  exit 1
fi
case "$(cat "$SC_RADIO_WORK/old-pool.log")" in
  *"relay socket allocation failed with DHCP and app listener"*) echo "Confirmed old pool fails; relay three-slot pool passes with DHCP reserved" ;;
  *) cat "$SC_RADIO_WORK/old-pool.log"; exit 1 ;;
esac
