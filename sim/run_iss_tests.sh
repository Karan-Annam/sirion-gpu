#!/usr/bin/env bash
# run_iss_tests.sh — assemble the test kernels and run the golden-ISS self-checking tests.
# Pure C++/Python (no Verilator). Native MSYS2 UCRT64 toolchain.
set -u
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) export PATH="/c/msys64/ucrt64/bin:$PATH" ;; esac
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build
PY="${PYTHON:-$(command -v python3 2>/dev/null || command -v python 2>/dev/null || echo python3)}"
CXX="${CXX:-g++}"
fail=0

echo "== assembling kernels =="
for k in vec_add saxpy_int abs loop_reduce; do
  if ! "$PY" scripts/asm.py "tests/kernels/$k.s" -o "build/$k.gpubin"; then
    echo "[FAIL] asm $k"; fail=1
  fi
done

echo "== building test_iss =="
if ! "$CXX" -std=c++17 -O2 -Wall -Wextra -Isim/iss -Isim/tests \
      sim/tests/test_iss.cpp -o build/test_iss.exe; then
  echo "[FAIL] compile test_iss"; exit 1
fi

echo "== running test_iss =="
./build/test_iss.exe || fail=1

echo "======================================================================"
if [ "$fail" -eq 0 ]; then echo "ALL ISS TESTS PASSED"; else echo "SOME ISS TESTS FAILED"; fi
exit $fail
