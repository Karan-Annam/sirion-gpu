# FPGA synthesis readiness (M22)

Sirion's RTL is written to the project's FPGA-portable conventions (synchronous active-high
reset, ANSI ports, one clock, no latches, no tri-states outside the pad ring, sim-only code
gated). This document records the synthesis-readiness check and what a real FPGA bring-up
would change.

## The check (`synth/synth.sh`, run from WSL/Linux)

Open-source flow: **sv2v** (SystemVerilog → Verilog-2005) then **yosys**.

```bash
# one-time tool setup (Ubuntu/WSL)
sudo apt install yosys unzip
wget https://github.com/zachjs/sv2v/releases/download/v0.0.12/sv2v-Linux.zip
unzip sv2v-Linux.zip && sudo cp sv2v-Linux/sv2v /usr/local/bin/

bash synth/synth.sh        # writes build/synth/{sirion.v, elab_report.txt, cu_synth.txt}
```

1. `sv2v -DSYNTHESIS` converts the full RTL with the synthesis view selected. Everything
   under `` `ifndef SYNTHESIS `` (SVA assertions, debug-only logic) drops out. This is the
   same macro a vendor flow would define.
2. yosys **elaborates the entire GPU** (`gpu_gfx_top`: CU array + L1s + L2 + dispatcher +
   rasterizer + graphics sequencer + memories), proving the design is structurally
   synthesizable, and reports design statistics (`elab_report.txt`). Measured:
   **77,517 cells / 84,406 wires**, including 3,442 adders, 684 multipliers, 78 dividers,
   9,304 comparators, 45,086 muxes, and 26 `$mem_v2` memory macros (the BRAM candidates).
3. yosys runs **generic gate-level synthesis of the execution units** (`fp_alu`, `sfu`,
   `alu`, `warp_sched`) to technology-independent netlists (`cu_synth.txt`). Measured:
   **fp_alu 12,138 cells · sfu 34,820 · alu 12,069 · warp_sched 87** (per lane; the SFU's
   size is dominated by its unrolled integer divider/√, a device build would pipeline
   and/or share SFUs across lanes at quarter rate, as real GPUs do). Gate-mapping the
   *whole* CU in one pass is deliberately not done here: it is dominated by flattening the
   131-Kbit register file into FF+mux trees, exactly the structure a device flow avoids
   by BRAM-mapping it (see the table below).

## What a real device build would change (documented, deliberate)

These are simulation-model conveniences that a device flow replaces; none change the
architecture:

| simulation model | FPGA implementation |
|------------------|---------------------|
| `gmem` as an on-chip array with combinational line reads | external DDR through the memory controller; the L2's line-granular request/ack interface is already the right seam |
| combinational reads of `smem`/`cbank`/texture/framebuffer memories | registered-read BRAMs (add one pipeline stage at each consumer; the barrel pipeline's stage structure has room) |
| flop-based register file (`regfile.sv`) | BRAM/LUTRAM banks; the module's bank-conflict port exists precisely for this conversion |
| single-cycle integer divide in `pinterp`/`zi` (rasterizer) and `f_rcp` (SFU) | multi-cycle pipelined dividers (the one-pixel-per-cycle scan FSM already tolerates longer per-pixel latency) |
| `sirion_run_rtl` host access via Verilator ports | an AXI-lite/UART bridge driving the same imem/cbank/gmem/launch/draw port protocol |

## Conventions that made this near-free

- Every memory is written synchronously; only reads are combinational (a mechanical rework).
- All state is in `always_ff` with synchronous reset; `always_comb` everywhere else, no
  inferred latches (yosys `proc` reports none).
- SVA, `$display`, and tracing live under `` `ifndef SYNTHESIS ``.
- No delays, no forces, no hierarchical references, one clock domain.
