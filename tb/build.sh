#!/usr/bin/env sh
# Builds the Verilator cosimulator into build/cosim/cosim.
set -eu
root="$(cd "$(dirname "$0")/.." && pwd)"
renice -n 19 -p $$ >/dev/null 2>&1 || true
mkdir -p "$root/build"
verilator --cc --exe --build -j "${JOBS:-$(nproc)}" -O3 --x-assign fast --x-initial fast \
    --top-module brainfunk_soc -DBRAINFUNK_RETIRE_PORT \
    --Mdir "$root/build/cosim" \
    -CFLAGS "-std=c++20 -O2 -I$root -I$root/tests" \
    "$root/rtl/brainfunk_core.sv" "$root/rtl/brainfunk_predict.sv" "$root/rtl/brainfunk_mem.sv" "$root/rtl/brainfunk_cache.sv" \
    "$root/rtl/sdram_ctrl.sv" "$root/tb/sdram_model.sv" "$root/rtl/brainfunk_soc.sv" \
    "$root/tb/cosim.cpp" "$root/libbrainfunk.cpp" "$root/optimizer.cpp" "$root/llvm_codegen.cpp" \
    -o cosim
