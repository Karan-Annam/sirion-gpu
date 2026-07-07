# Sirion datapath primitives (M2) — design notes

The M2 modules are the building blocks of the Compute Unit's execute path. Each is a small,
independently-verified leaf module (no integrating top yet — that arrives in M3). All are checked
against the same golden C++ functions the ISS uses, so RTL and the golden model provably agree.

| Module | File | Kind | Golden reference | Random checks |
|--------|------|------|------------------|---------------|
| Integer ALU | `rtl/compute/alu.sv` | combinational | `alu_ref()` (isa.hpp) | 40k |
| Vector register file | `rtl/compute/regfile.sv` | sequential | C++ `gold[reg][lane]` | 167k |
| Instruction decoder | `rtl/compute/decode.sv` | combinational | `decode_ref()` (isa.hpp) | 629k |
| Scoreboard | `rtl/compute/scoreboard.sv` | sequential | C++ busy-vector | 10k |
| Sync FIFO | `rtl/common/sync_fifo.sv` | sequential | `std::deque` | 31k |

## 1. Integer ALU (`alu.sv`)

Combinational, scalar 32-bit — one lane's datapath; the SIMT execute stage (M4) replicates it
across a warp's 32 lanes. Inputs: `opcode`, `a`, `b`. The caller selects `b` (rs2 or immediate)
and, for MOV/MOVI, passes the pass-through value in `a`, so the ALU only needs the opcode plus two
operands. `is_alu` distinguishes handled ALU opcodes from everything else (so execute routes non-ALU
ops elsewhere). Ops: add/sub, mul (low32) + mulh (signed high32), and/or/xor/not, shl/shr/sra
(shamt = b[4:0]), slt/sltu, min/max, seq, mov. Mirrors `alu_ref()` exactly.

## 2. Vector register file (`regfile.sv`)

Holds 16 registers, each a full warp (32 lanes × 32 bits). One **predicated** write port (per-lane
`wmask` for masked SIMT writeback) and two **registered** read ports (1-cycle latency, like BRAM).

- **No write bypass:** a concurrent read+write to the same register returns the OLD value; the
  execute stage forwards when needed (M3). The golden model reproduces this exactly.
- **Banking:** registers map to `NUM_BANKS` (=4) banks by `reg_id % NUM_BANKS`. Storage here is
  flip-flop based (16 regs is cheap) and always returns correct data on both ports; `bank_conflict`
  flags when the two read addresses hit the same bank (differing regs) so the operand collector (M3)
  can serialize once we move to single-ported BRAM banks. This module models storage + surfaces the
  conflict signal; it does not itself serialize.

Timing: drive `raddr*`/`ren*` in cycle *t*; `rdata*` valid in cycle *t+1*.

## 3. Instruction decoder (`decode.sv`)

Combinational. Splits a 32-bit instruction into register/immediate fields and control signals
(`reg_write`, `is_alu`/`is_load`/`is_store`/`is_branch`/`is_ssy`/`is_setp`/`is_rdsr`, `uses_rs1/2`,
`alu_a_imm`/`alu_b_imm`, `mem_shared`, format-selected sign-extended `imm`). Class flags are mutually
exclusive (SVA-checked). Mirrors `decode_ref()`; the test drives every opcode plus 20k fully-random
words (reserved encodings decode to all-flags-false, imm 0).

## 4. Scoreboard (`scoreboard.sv`)

Per-warp, one busy bit per register. Combinational `stall` on RAW (a source register busy) or WAW
(destination busy). `issue` sets `busy[rd]` (asserted by issue logic only when `!stall`); `wb_valid`
clears `busy[wb_rd]`. On a same-cycle set+clear of one register the set wins (otherwise precluded by
the stall). A single busy bit suffices because WAW stalls prevent a second in-flight write to a
register. SVA: stalls are justified by an actually-busy register; no double-issue to a busy register.

## 5. Sync FIFO (`sync_fifo.sv`)

Parameterized (WIDTH, DEPTH=power of two) circular buffer; `dout` combinationally reflects the head
and is valid when `!empty`; `pop` advances it; `push` writes when `!full`. Reused later for issue and
memory request queues. SVA: no overflow/underflow, count in range, never full&empty.

## Verification & limitations

All five are exercised with directed + seeded-random tests via `make sim`; SVA is on (`--assert`) and
waveforms are available (`--trace`). Not yet integrated into a pipeline (M3), no forwarding/operand
serialization yet (M3), FP/SFU excluded (M8). Scalar register file and additional read ports (for FMA
/ store-data) are provisioned in the ISA/package and added as the pipeline needs them.
