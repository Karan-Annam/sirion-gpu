#!/usr/bin/env bash
# synth.sh — FPGA synthesis-readiness check for the Sirion GPU (M22).
#
# Open-source flow: sv2v (SystemVerilog -> Verilog-2005) + yosys (synthesis).
# Run from WSL/Linux:   bash synth/synth.sh
#
# Produces build/synth/:
#   sirion.v        the converted netlist input (SYNTHESIS defined: SVA/trace gated out)
#   elab_report.txt yosys elaboration + design statistics for the FULL GPU (gpu_gfx_top)
#   cu_synth.txt    generic gate-level synthesis statistics for one compute unit
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build/synth

SV2V="${SV2V:-$(command -v sv2v || echo /tmp/sv2v-Linux/sv2v)}"
YOSYS="${YOSYS:-yosys}"
"$SV2V" --version >/dev/null || { echo "sv2v not found (set SV2V=...)"; exit 1; }
"$YOSYS" -V >/dev/null || { echo "yosys not found (set YOSYS=...)"; exit 1; }

RTL="rtl/sirion_pkg.sv rtl/common/counter.sv rtl/common/sync_fifo.sv
     rtl/compute/alu.sv rtl/compute/fp_alu.sv rtl/compute/sfu.sv
     rtl/compute/regfile.sv rtl/compute/decode.sv rtl/compute/scoreboard.sv
     rtl/scheduler/warp_sched.sv rtl/scheduler/cta_dispatch.sv
     rtl/cache/l1_dcache.sv rtl/cache/l2_cache.sv
     rtl/compute/cu_core.sv rtl/gpu_top.sv
     rtl/graphics/raster.sv rtl/graphics/gfx_seq.sv rtl/gpu_gfx_top.sv"

echo "== sv2v: SystemVerilog -> Verilog (SYNTHESIS defined) =="
"$SV2V" -DSYNTHESIS $RTL > build/synth/sirion.v || { echo "sv2v FAILED"; exit 1; }
wc -l build/synth/sirion.v

if [ "${STAGE2_ONLY:-0}" = "1" ]; then
  echo "== (STAGE2_ONLY: skipping the full-GPU elaboration pass) =="
else
echo "== yosys: elaborate + stats for the full GPU (gpu_gfx_top) =="
"$YOSYS" -p "
  read_verilog build/synth/sirion.v;
  hierarchy -check -top gpu_gfx_top;
  proc; opt_clean;
  memory_collect;
  stat
" > build/synth/elab_report.txt 2>&1 || { echo "yosys elaboration FAILED"; tail -20 build/synth/elab_report.txt; exit 1; }
grep -E "Number of cells|Number of wires" build/synth/elab_report.txt | head -4
fi

echo "== yosys: gate-level synthesis of the execution units (fp_alu, sfu, alu, warp_sched) =="
# The full-GPU elaboration above proves structural synthesizability; this stage maps the
# arithmetically-hardest leaf modules to a generic gate netlist for realistic gate counts.
# (Gate-mapping the whole CU is dominated by flattening the 131-Kbit register file into
# FF+mux trees — exactly what a device flow AVOIDS by BRAM-mapping it; see docs/FPGA.md.)
"$YOSYS" -p "
  read_verilog build/synth/sirion.v;
  hierarchy -check -top fp_alu;  synth -noabc -flatten; stat;
  design -reset;
  read_verilog build/synth/sirion.v;
  hierarchy -check -top sfu;     synth -noabc -flatten; stat;
  design -reset;
  read_verilog build/synth/sirion.v;
  hierarchy -check -top alu;     synth -noabc -flatten; stat;
  design -reset;
  read_verilog build/synth/sirion.v;
  hierarchy -check -top warp_sched; synth -noabc -flatten; stat
" > build/synth/cu_synth.txt 2>&1 || { echo "yosys unit synth FAILED"; tail -20 build/synth/cu_synth.txt; exit 1; }
grep -E "=== (fp_alu|sfu|alu|warp_sched)|Number of cells" build/synth/cu_synth.txt | head -12

echo "== OK: reports in build/synth/ =="
