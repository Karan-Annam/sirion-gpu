#!/usr/bin/env bash
# run_hl_tests.sh — compile high-level kernels (.k -> .s -> .gpubin) and run them on the ISS.
# This is the project's completion check: high-level source compiles down to custom assembly,
# then the custom binary, and runs correctly. Pure C++/Python (no Verilator).
set -u
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) export PATH="/c/msys64/ucrt64/bin:$PATH" ;; esac
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build
PY="${PYTHON:-$(command -v python3 2>/dev/null || command -v python 2>/dev/null || echo python3)}"
CXX="${CXX:-g++}"
fail=0

compile_k() {   # <kfile> <out-base>
  echo "[hl] $1"
  if ! "$PY" scripts/sirc.py "$1" -o "build/$2.s"; then echo "[FAIL] sirc $1"; fail=1; return; fi
  if ! "$PY" scripts/asm.py "build/$2.s" -o "build/$2.gpubin"; then echo "[FAIL] asm $2"; fail=1; fi
}

echo "== compiling high-level kernels =="
compile_k examples/kernels/vec_add.k     hl_vec_add
compile_k examples/kernels/saxpy.k       hl_saxpy
compile_k examples/kernels/strided_sum.k hl_strided
# M21 language features: floats, inline funcs + SFU, shared+barrier, atomics, spilling
compile_k examples/kernels/saxpyf.k        hl_saxpyf
compile_k examples/kernels/normalize.k     hl_normalize
compile_k examples/kernels/reduce_shared.k hl_reduce_shared
compile_k examples/kernels/hist.k          hl_hist
compile_k examples/kernels/spill.k         hl_spill
compile_k examples/kernels/gen_checker.k   hl_gen_checker

echo "== building test_hl =="
if ! "$CXX" -std=c++17 -O2 -Wall -Wextra -Isim/iss -Isim/tests \
      sim/tests/test_hl.cpp -o build/test_hl.exe; then
  echo "[FAIL] compile test_hl"; exit 1
fi

echo "== running test_hl =="
./build/test_hl.exe || fail=1

echo "======================================================================"
if [ "$fail" -eq 0 ]; then echo "ALL HL TESTS PASSED"; else echo "SOME HL TESTS FAILED"; fi
exit $fail
