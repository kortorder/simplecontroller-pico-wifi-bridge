#!/bin/sh
set -eu
cd "$(dirname "$0")"
target="${TMPDIR:-/tmp}/sc-chair-relay-checks-$$"
trap 'rm -f "$target"' EXIT HUP INT TERM
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -Iinclude src/scx2.cpp tests/chair_relay_checks.cpp -o "$target"
"$target"
