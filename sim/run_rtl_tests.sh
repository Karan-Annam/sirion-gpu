#!/usr/bin/env bash
# Verilate + build + run each self-checking RTL unit test.
#
# MSYS2 quirks handled below (../docs/BUILDING.md): PATH must put ucrt64 first
# or g++ fails silently; call verilator_bin.exe directly (broken Perl wrapper).
set -u
# host toolchain: native MSYS2 (Windows Verilator) vs Linux/WSL (native verilator)
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    export PATH="/c/msys64/ucrt64/bin:$PATH"        # ucrt64 before mingw64 (docs/BUILDING.md)
    export VERILATOR_ROOT="C:/msys64/ucrt64/share/verilator"
    VERILATOR="${VERILATOR:-verilator_bin.exe}"     # Perl wrapper is broken; call the binary
    ;;
  *)  VERILATOR="${VERILATOR:-verilator}" ;;        # Linux / WSL: native verilator
esac
PY="${PYTHON:-$(command -v python3 2>/dev/null || command -v python 2>/dev/null || echo python3)}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OBJ="obj_dir"
mkdir -p build
WARN="-Wall -Wno-UNUSED -Wno-DECLFILENAME -Wno-IMPORTSTAR -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PINCONNECTEMPTY -Wno-VARHIDDEN"
fail=0

# build_run <name> <top-module> <testsrc.cpp> <rtl files...>
# Set RTL_ONLY="name1 name2" to run a subset (used to chunk long CI runs).
build_run() {
  local name="$1" top="$2" testsrc="$3"; shift 3
  if [ -n "${RTL_ONLY:-}" ]; then
    case " ${RTL_ONLY} " in *" ${name} "*) ;; *) return ;; esac
  fi
  local rtl="$*"
  # Absolute path for the C++ testbench so the generated sub-make (which runs from the
  # Mdir) finds it on any Verilator version. On MSYS, hand the .exe a Windows-style path.
  local tsrc="$ROOT/$testsrc"
  case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) tsrc="$(cygpath -m "$tsrc")" ;; esac
  echo "======================================================================"
  echo "[$name] verilate + build (top=$top)"
  rm -rf "$OBJ/$name"
  mkdir -p "$OBJ/$name"
  # verilate, then build with an explicit parallel make (verilator's --build -j does not
  # reliably parallelize the sub-make on every host; the M17 model is large)
  # --unroll-count: the L2 reset loop iterates SETS=256 times; Verilator 5.020's default
  # unroll limit rejects it (BLKLOOPINIT) while 5.040 accepts — set it explicitly.
  if ! $VERILATOR --cc --exe --trace --assert -j 0 --unroll-count 1024 \
        $WARN ${VEXTRA:-} --top-module "$top" --Mdir "$OBJ/$name" -o "${name}_sim" \
        --CFLAGS "-std=c++17" \
        $rtl "$tsrc"; then
    echo "[FAIL] $name (verilate)"; fail=1; return
  fi
  local jobs; jobs=$( (nproc 2>/dev/null || echo 8) )
  if ! make -j "$jobs" -C "$OBJ/$name" -f "V${top}.mk" >/dev/null; then
    echo "[FAIL] $name (build)"; fail=1; return
  fi
  echo "[$name] run"
  local exe="$OBJ/$name/${name}_sim"
  [ -x "$exe" ] || exe="$exe.exe"
  if ! "./$exe"; then
    echo "[FAIL] $name (run)"; fail=1; return
  fi
}

# ---- M0 ---------------------------------------------------------------------
# counter is standalone (no package import) — omit sirion_pkg.sv so its waveform
# isn't cluttered by the package's (constant) parameter/enum "signals".
build_run counter counter sim/tests/test_counter.cpp \
    rtl/common/counter.sv

# ---- M2 (added as modules land) --------------------------------------------
build_run alu        alu        sim/tests/test_alu.cpp        rtl/sirion_pkg.sv rtl/compute/alu.sv
build_run regfile    regfile    sim/tests/test_regfile.cpp    rtl/sirion_pkg.sv rtl/compute/regfile.sv
build_run decode     decode     sim/tests/test_decode.cpp     rtl/sirion_pkg.sv rtl/compute/decode.sv
build_run scoreboard scoreboard sim/tests/test_scoreboard.cpp rtl/sirion_pkg.sv rtl/compute/scoreboard.sv
build_run sync_fifo  sync_fifo  sim/tests/test_sync_fifo.cpp  rtl/sirion_pkg.sv rtl/common/sync_fifo.sv

# ---- M3/M4: assemble core kernels, then run the integrated core vs the ISS ----
for k in m3_arith m3_pred m3_ids m4_abs m4_nested m4_loop m6_shared block_shuffle vec_add saxpy_int saxpy_float loop_reduce histogram rsqrt_map vs_persp fs_texmod; do
  "$PY" scripts/asm.py "tests/kernels/$k.s" -o "build/$k.gpubin" || { echo "[FAIL] asm $k"; fail=1; }
done
# high-level vec_add through the full compiler for an end-to-end HL -> RTL check
"$PY" scripts/sirc.py examples/kernels/vec_add.k -o build/hl_vec_add.s \
  && "$PY" scripts/asm.py build/hl_vec_add.s -o build/hl_vec_add.gpubin \
  || { echo "[FAIL] compile hl_vec_add"; fail=1; }
# after M17 all kernel tests target gpu_top (CU L1s -> shared L2 -> global memory)
GPU_RTL="rtl/sirion_pkg.sv rtl/compute/alu.sv rtl/compute/fp_alu.sv rtl/compute/sfu.sv \
    rtl/compute/regfile.sv rtl/compute/decode.sv rtl/scheduler/warp_sched.sv \
    rtl/cache/l1_dcache.sv rtl/cache/l2_cache.sv rtl/compute/cu_core.sv \
    rtl/scheduler/cta_dispatch.sv rtl/gpu_top.sv"
build_run cu_core gpu_top sim/tests/test_cu_core.cpp $GPU_RTL

# ---- M12/M14/M16: dispatch + cache/coalescer + atomics/SFU system tests ----
build_run gpu_top gpu_top sim/tests/test_gpu_top.cpp $GPU_RTL

# ---- M17: four CUs sharing the L2 (concurrency + atomics-at-L2 + scaling) ----
VEXTRA="-GNUM_CU=4" build_run multi_cu gpu_top sim/tests/test_multi_cu.cpp $GPU_RTL

# ---- M19/M20: programmable shaders + the hardware-sequenced pipeline ----
"$PY" scripts/sirc.py examples/kernels/gen_checker.k -o build/hl_gen_checker.s \
  && "$PY" scripts/asm.py build/hl_gen_checker.s -o build/hl_gen_checker.gpubin \
  || { echo "[FAIL] compile gen_checker"; fail=1; }
GFX_RTL="$GPU_RTL rtl/graphics/raster.sv rtl/graphics/gfx_seq.sv rtl/gpu_gfx_top.sv"
build_run shader  gpu_gfx_top sim/tests/test_shader_pipeline.cpp $GFX_RTL
build_run gfxpipe gpu_gfx_top sim/tests/test_gfx_pipeline.cpp   $GFX_RTL

# ---- M7/M8: memory + scheduler + floating point (verified standalone) ----
build_run l1_cache   l1_cache   sim/tests/test_l1_cache.cpp   rtl/sirion_pkg.sv rtl/cache/l1_cache.sv
# ---- M14: line-granular L1 D$ for the coalesced LSU path ----
build_run l1_dcache  l1_dcache  sim/tests/test_l1_dcache.cpp  rtl/sirion_pkg.sv rtl/cache/l1_dcache.sv
build_run warp_sched warp_sched sim/tests/test_warp_sched.cpp rtl/scheduler/warp_sched.sv
build_run fp_alu     fp_alu     sim/tests/test_fp_alu.cpp     rtl/sirion_pkg.sv rtl/compute/fp_alu.sv
# ---- M16: special-function unit ----
build_run sfu        sfu        sim/tests/test_sfu.cpp        rtl/sirion_pkg.sv rtl/compute/sfu.sv

# ---- M9: graphics rasterizer (renders a cube; verified pixel-exact vs the golden) ----
build_run raster raster sim/tests/test_raster.cpp rtl/sirion_pkg.sv rtl/graphics/raster.sv

echo "======================================================================"
if [ "$fail" -eq 0 ]; then echo "ALL RTL TESTS PASSED"; else echo "SOME RTL TESTS FAILED"; fi
exit $fail
