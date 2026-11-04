#!/bin/bash
set -euo pipefail
SC_MCP_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SC_MCP_WORK="${SC_BRIDGE_MCP_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}"/simplecontroller-mcp4251-checks-XXXXXX)}"
mkdir -p "$SC_MCP_WORK"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$SC_MCP_ROOT/include" -I "$SC_MCP_ROOT/firmware" \
  "$SC_MCP_ROOT/src/scx2.cpp" "$SC_MCP_ROOT/tests/mcp4251_checks.cpp" -o "$SC_MCP_WORK/mcp4251-checks"
"$SC_MCP_WORK/mcp4251-checks"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$SC_MCP_ROOT/tests/fake_pico" -I "$SC_MCP_ROOT/include" -I "$SC_MCP_ROOT/firmware" \
  "$SC_MCP_ROOT/tests/pico_trigger_gate_checks.cpp" -o "$SC_MCP_WORK/pico-trigger-gate-checks"
"$SC_MCP_WORK/pico-trigger-gate-checks"
