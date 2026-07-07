#!/usr/bin/env bash
# run_all.sh — build + run the full Sirion test suite (golden ISS + RTL units).
#
# Works from BOTH native MSYS2 (Windows Verilator) and WSL/Linux (native verilator);
# the sub-scripts auto-detect the host. Env toggles: SKIP_RTL=1 -> ISS tests only.
set -u
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    export PATH="/c/msys64/ucrt64/bin:$PATH"
    export VERILATOR_ROOT="C:/msys64/ucrt64/share/verilator" ;;
esac
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
fail=0

echo "########## ISS golden-model tests ##########"
bash sim/run_iss_tests.sh || fail=1

echo "########## high-level compiler tests ##########"
bash sim/run_hl_tests.sh || fail=1

if [ "${SKIP_RTL:-0}" != "1" ]; then
  echo "########## RTL unit tests (Verilator) ##########"
  bash sim/run_rtl_tests.sh || fail=1
fi

echo "################################################"
if [ "$fail" -eq 0 ]; then echo "SIRION: ALL TESTS PASSED"; else echo "SIRION: FAILURES PRESENT"; fi
exit $fail
