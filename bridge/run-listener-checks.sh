#!/bin/bash
set -euo pipefail
SC_RETRY_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SC_RETRY_WORK="${SC_BRIDGE_RETRY_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}"/simplecontroller-listener-checks-XXXXXX)}"
mkdir -p "$SC_RETRY_WORK"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$SC_RETRY_ROOT/firmware" "$SC_RETRY_ROOT/tests/listener_retry_checks.cpp" -o "$SC_RETRY_WORK/listener-checks"
"$SC_RETRY_WORK/listener-checks"
