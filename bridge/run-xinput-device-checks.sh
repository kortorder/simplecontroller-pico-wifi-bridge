#!/bin/bash
set -euo pipefail
SC_XINPUT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SC_XINPUT_WORK="$(mktemp -d "${TMPDIR:-/tmp}"/simplecontroller-xinput-device-XXXXXX)"
mkdir -p "$SC_XINPUT_WORK/shim/device"
touch "$SC_XINPUT_WORK/shim/tusb.h" "$SC_XINPUT_WORK/shim/device/usbd_pvt.h"
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I "$SC_XINPUT_ROOT/include" -I "$SC_XINPUT_WORK/shim" \
  "$SC_XINPUT_ROOT/tests/xinput_device_checks.cpp" -o "$SC_XINPUT_WORK/checks"
"$SC_XINPUT_WORK/checks"
