#!/bin/bash
set -euo pipefail
SC_DISCOVERY_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SC_DISCOVERY_WORK="$(mktemp -d "${TMPDIR:-/tmp}"/simplecontroller-discovery-checks-XXXXXX)"
cd "$SC_DISCOVERY_ROOT"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I bridge/include bridge/src/scx2.cpp bridge/tests/udp_loopback_peer.cpp -o "$SC_DISCOVERY_WORK/udp-peer"
SC_DISCOVERY_PEER="$SC_DISCOVERY_WORK/udp-peer" python3 bridge/tests/discovery_checks.py
