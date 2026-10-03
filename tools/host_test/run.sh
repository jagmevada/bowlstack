#!/usr/bin/env bash
# Builds and runs the host tests for the load-scale core -- no hardware, no
# PlatformIO. Deliberately NOT under test/: a bare `pio test` would try to build
# anything there for the default ESP32 environment.
#
#   bash tools/host_test/run.sh
#
# Uses the same MSYS2 mingw64 toolchain the `sim` environment uses
# (scripts/sim_toolchain.py); set MSYS2_ROOT to override C:\msys64.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
msys="${MSYS2_ROOT:-/c/msys64}"
mingw="$msys/mingw64/bin"
if [ -x "$mingw/g++.exe" ]; then
  export PATH="$mingw:$PATH"   # also puts libstdc++ on PATH for the run
fi

out="$(mktemp -d)/load_scale_test.exe"
g++ -std=c++17 -O1 -Wall -Wextra -Werror \
    -I "$root/include" \
    "$root/src/loadcell/load_scale.cpp" \
    "$here/test_load_scale.cpp" \
    -o "$out"
"$out"
