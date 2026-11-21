#!/bin/bash
set -euo pipefail
BRIDGE_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE_CHECK_DIR="${SC_BRIDGE_BUILD_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}"/simplecontroller-scx2-checks-XXXXXX)}"
mkdir -p "$BRIDGE_CHECK_DIR"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$BRIDGE_ROOT/include" "$BRIDGE_ROOT/src/scx2.cpp" "$BRIDGE_ROOT/tests/checks.cpp" \
  -o "$BRIDGE_CHECK_DIR/scx2-checks"
"$BRIDGE_CHECK_DIR/scx2-checks" "$BRIDGE_ROOT/fixtures"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$BRIDGE_ROOT/include" "$BRIDGE_ROOT/src/scx2.cpp" "$BRIDGE_ROOT/tests/pressure_checks.cpp" \
  -o "$BRIDGE_CHECK_DIR/scx2-pressure-checks"
"$BRIDGE_CHECK_DIR/scx2-pressure-checks" "$BRIDGE_ROOT/fixtures/pressure"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$BRIDGE_ROOT/include" "$BRIDGE_ROOT/src/scx2.cpp" "$BRIDGE_ROOT/tests/usb_trigger_checks.cpp" \
  -o "$BRIDGE_CHECK_DIR/scx2-usb-trigger-checks"
"$BRIDGE_CHECK_DIR/scx2-usb-trigger-checks" "$BRIDGE_ROOT/fixtures/usb-buttons"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$BRIDGE_ROOT/include" "$BRIDGE_ROOT/src/scx2.cpp" "$BRIDGE_ROOT/tests/xinput_checks.cpp" \
  -o "$BRIDGE_CHECK_DIR/scx2-xinput-checks"
"$BRIDGE_CHECK_DIR/scx2-xinput-checks" "$BRIDGE_ROOT/fixtures/usb-pressure"
