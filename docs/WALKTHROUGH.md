# Sirion, the build log, milestone by milestone

The "understand exactly what exists" tour. It walks the files built in the
foundation milestones (M0–M2) in depth, then gives a per-milestone report for
everything after, **what it does**, **how it works**, how it **fits together**,
and the bugs each stage's tests caught along the way.

**Where things live:** RTL in `rtl/`, the C++ golden model in `sim/iss/`, test harness + testbenches in
`sim/`, the assembler + tools in `scripts/`, kernels in `tests/kernels/`, docs in `docs/`. Total ≈ 2,240
lines of source across 33 files.

**The one-sentence mental model:** one ISA is written down once per purpose, as C++ (`sim/iss/isa.hpp`) and
as SystemVerilog (`rtl/sirion_pkg.sv`); the **assembler** turns text kernels into binaries, the **ISS** runs
them as the golden answer, and the **RTL modules** are the hardware pieces that (from M3) will re-run the
same binaries and be diffed against that golden answer.

---

## M0: Foundation & toolchain (prove the whole flow on a trivial module)

| File | What it does |
|------|--------------|
| `rtl/sirion_pkg.sv` | The SystemVerilog "header" for the whole GPU. |
| `rtl/common/counter.sv` | A trivial counter, the module used to prove the build works. |
| `sim/tests/test_framework.hpp` | A tiny, dependency-free C++ test framework. |
| `sim/rtl_driver.hpp` | The wrapper that clocks/resets/traces a Verilated DUT. |
| `sim/tests/test_counter.cpp` | The self-checking testbench for the counter. |
| `Makefile`, `sim/run_rtl_tests.sh`, `run_all.sh` | The build/test entry points. |
| `README.md`, `docs/BUILDING.md` | Quick-start and the build-gotchas log. |

**`rtl/sirion_pkg.sv`**, a SystemVerilog `package` compiled before everything else. It holds the machine's
sizing parameters (`WARP_SIZE=32`, `XLEN=32` data width, `NUM_VREGS=16`, `NUM_PREDS=4`), the core types
(`word_t` = 32-bit lane word, `warp_vec_t` = a full warp of 32 words, `insn_t`, masks), the **full opcode
enum** (`opcode_e`), the compare/special-register constants, and a set of **field-extractor functions**
(`insn_opcode`, `insn_rd`, `insn_imm15`, …) that pull fields out of a 32-bit instruction. It is the RTL twin
of `sim/iss/isa.hpp`, the two must agree, which the decode test enforces.

**`rtl/common/counter.sv`**, a parametric saturating up-counter with `en`/`load`/`tick`. It exists only to
be the smallest useful synchronous module, so we could prove the entire chain (Verilator build → `--trace`
waveform → `--assert` SVA → C++ self-checking) before writing real RTL. It sets the project conventions:
ANSI ports, **synchronous active-high reset**, registered outputs, `tick` as a 1-cycle pulse, and inline
SVA (`tick` only pulses on wrap; `count` is never X).

**`sim/tests/test_framework.hpp`**, the house test harness, no GoogleTest. You write `TEST(name){ ... }`;
a static `Registrar` auto-adds it to a list; `CHECK(cond)` / `CHECK_EQ(a,b)` throw on failure; `run_all()`
runs every test, prints `[PASS]`/`[FAIL]` and a check count, and returns the number of failures (so the
process exit code drives the shell scripts). Every testbench in the project uses this.

**`sim/rtl_driver.hpp`**, a `Sim<DUT>` template that wraps any Verilated module. It gives each test its own
private `VerilatedContext` **and owns the DUT** (constructing it on that context) so independent tests in one
executable never collide, this was necessary because sharing the global context breaks tracing on the second
model. It provides `tick()` (a full two-phase clock: `clk=0; eval; clk=1; eval`), `reset(n)`, and
`open_trace()/close_trace()` for VCD dumping. It also defines the `sc_time_stamp()` stub Verilator's trace
code needs at link time (`__attribute__((used))` forces the symbol to be emitted).

**`sim/tests/test_counter.cpp`**, drives the counter and checks it: basic increment, enable-gating, load
priority, wrap+tick, plus `counter_waveform` (a ~45-cycle scenario that dumps `build/counter.vcd`, the trace
`make waves`/`make wavetext` shows).

**Build/test entry points**, `Makefile` (targets `sim`/`test`/`lint`/`waves`/`wavetext`/`clean`; RTL listed
in explicit compile order, package first; auto-detects `python3`/`python`), `sim/run_rtl_tests.sh` (a
`build_run <name> <top> <testsrc> <rtl…>` function that Verilates each module with `--trace --assert`,
compiles, and runs it), and `run_all.sh` (runs the ISS suite then the RTL suite). All of them set the MSYS2
env (`PATH` with ucrt64 first, `VERILATOR_ROOT`), see `BUILDING.md` for why the PATH order matters.

---

## M1: The ISA, the golden simulator, and the assembler

| File | What it does |
|------|--------------|
| `docs/ISA.md` | The authoritative instruction-set reference. |
| `sim/iss/isa.hpp` | ISA encoding **in C++** + the shared golden functions. |
| `sim/iss/gpubin.hpp` | Reader for the `.gpubin` object format. |
| `sim/iss/iss.hpp` | The functional simulator (the golden model). |
| `scripts/asm.py` | The two-pass assembler (text → `.gpubin`). |
| `scripts/vcd2txt.py` | A no-GUI VCD viewer (renders waveforms as text). |
| `tests/kernels/*.s` | Four real kernels. |
| `sim/tests/test_iss.cpp`, `sim/run_iss_tests.sh` | Assemble + run + check the kernels. |

**`sim/iss/isa.hpp`**, the C++ definition of the ISA and the project's single source of truth for
"what an instruction means." It contains: the `Opcode`/`Cmp`/`Sreg` enums; `struct Insn` (a thin decoder
with bit-field accessors like `.opcode()`, `.rd()`, `.imm15()`); `enc_*` encoder helpers; and two **golden
functions** that are reused by the RTL testbenches so hardware and model can't drift, `alu_ref(op,a,b)` (the reference integer ALU) and `decode_ref(raw)` (the reference control decode, returning
a `DecodeInfo` of fields + flags).

**`sim/iss/gpubin.hpp`**, defines the `.gpubin` layout (32-byte little-endian header: magic `SGPU`,
version/flags, instruction count, constant count, entry, metadata; then the code words, then the constant
words) and `load_gpubin()` to read one into a `GpuBin` struct.

**`sim/iss/iss.hpp`**, the **ISS**, the heart of M1. The `Iss` class holds global memory (a byte vector),
the constant bank (kernel parameters), and grid/block dimensions. `run()` loops over every block and every
warp; **`run_warp()`** is where the SIMT semantics live: it maintains the reconvergence **stack of frames**
`{mask, pc, rpc, pending_rpc}`, evaluates each instruction's **guard** to get the active lanes, and executes
every opcode per-lane, ALU (via `alu_ref`), `ISETP` (writes a predicate register), `LDG/STG/LDC` (memory),
`RDSR` (special registers, computed in `sreg()`), and the branch logic that pushes/pops frames on divergence
(the classic IPDOM stack algorithm). Reading `run_warp()` is the best way to *understand*
SIMT because it's ~120 lines of plain C++.

**`scripts/asm.py`**, the assembler. Pass 1 strips comments, records label→address, and parses each line
(optional `@P`/`@!P` guard prefix, mnemonic, operands); pass 2 encodes each instruction by format using an
opcode table that mirrors `isa.hpp`, resolves branch offsets, and writes the `.gpubin`. Operand parsers
handle registers (`R0`), predicates (`P1`), immediates (dec/hex/`#`), memory (`[R3+16]`), constants (`c[3]`),
and special-register names.

**`scripts/vcd2txt.py`**, a small utility (added while fixing a WSL GTKWave problem) that parses a VCD and
prints it as a **per-clock-cycle table** or an **ASCII waveform**, no X server or GUI needed. Run via
`make wavetext`.

**`tests/kernels/*.s`**, four real programs that exercise the ISA: `vec_add` (basic SIMT + a range-guard
divergence), `saxpy_int` (adds a multiply), `abs` (**nested** divergence, a range guard around a sign
branch), and `loop_reduce` (a **backward-branch loop**).

**`sim/tests/test_iss.cpp` / `sim/run_iss_tests.sh`**, the M1 verification. The script assembles each kernel
to `build/*.gpubin`; the C++ test loads each one, sets up a grid + input buffers + parameters, runs the ISS,
and checks global memory against an **independent** C++ computation (e.g. `out[i] == 3*i`), including a
range-guard test proving out-of-range lanes were never written. 5 tests, 356 checks.

---

## M2: The datapath RTL (the Compute Unit's building blocks)

| File | What it does | Verified against |
|------|--------------|------------------|
| `rtl/compute/alu.sv` | Integer ALU, one lane, combinational | `alu_ref()` (40k random) |
| `rtl/compute/regfile.sv` | Banked vector register file | C++ `gold[reg][lane]` (167k) |
| `rtl/compute/decode.sv` | Instruction decoder | `decode_ref()` (629k) |
| `rtl/compute/scoreboard.sv` | RAW/WAW hazard tracker | C++ busy vector (10k) |
| `rtl/common/sync_fifo.sv` | Reusable FIFO | `std::deque` (31k) |
| `docs/datapath.md` | Design notes for the above |, |

**`rtl/compute/alu.sv`**, a purely combinational, scalar 32-bit integer ALU (add/sub, mul + mulh, logic,
shifts, set-less-than, min/max, seq, mov). It's *one lane's* datapath, the SIMT execute stage (M4) will
instantiate 32 of them. The caller pre-selects operand `b` (register or immediate); an `is_alu` output flags
whether this opcode is one the ALU handles. Its behavior is byte-for-byte the `alu_ref()` the ISS uses.

**`rtl/compute/regfile.sv`**, the vector register file: 16 registers, each a full warp (32 lanes × 32 bits).
It has one **predicated write port** (a per-lane `wmask`, for masked SIMT writeback) and two **registered
read ports** (1-cycle latency, like real SRAM, with **no write bypass**, a concurrent read+write to the same
register returns the old value; forwarding is the execute stage's job in M3). Registers map to 4 banks;
`bank_conflict` flags when two reads hit the same bank so the future operand collector can serialize.

**`rtl/compute/decode.sv`**, the combinational hardware decoder. It splits the instruction into fields and
control flags (`is_alu`, `is_load`, `is_store`, `is_branch`, `reg_write`, immediate select, operand-source
select, …), mirroring `decode_ref()` exactly. The test drives every opcode plus 20k random words and compares
every output field to the golden, so the SV and C++ decoders are provably identical.

**`rtl/compute/scoreboard.sv`**, one busy bit per register for a warp. It asserts `stall` combinationally if
a needed source register has an in-flight write (RAW) or the destination is busy (WAW); `issue` sets the
destination bit, `wb_valid` clears it. This is what lets the pipeline (M3) know when it's safe to issue a
dependent instruction.

**`rtl/common/sync_fifo.sv`**, a parametric circular FIFO (power-of-two depth) with full/empty/count and
overflow/underflow assertions. Built now because the scheduler and memory stages (M5, M7) will need queues.

---

## M3: the single-warp integer core (first RTL integration) ✅

**What's done:** the M2 blocks are now wired into a *running* RTL core that executes a full warp
(32 lanes) of a **straight-line integer kernel** and is checked **bit-for-bit against the golden ISS**, the first time hardware executes a real program. Four kernel/context cases pass, 2064 checks, in
both MSYS2 (Verilator 5.040) and WSL (5.020).

**Scope (per the M3 milestone):** supported = NOP, integer ALU (R/I), MOV/MOVI/NOT, RDSR (1D special
registers), ISETP/ISETPI (writes predicate regs), full per-lane **predication**, and EXIT. Deferred =
branches/divergence (M4) and memory (M5). The core carries assertions that fire if a kernel uses a
load/store/branch, so the scope is enforced, not assumed.

**How the core works** (`rtl/compute/cu_core.sv`): a simple in-order **multicycle** core, `FETCH → READ → EXEC`, one instruction every 3 cycles. Because instructions don't overlap, there are
no data hazards to forward or stall (the scoreboard is verified standalone in M2 and gets integrated
with the overlapped multi-warp issue in M4). Each cycle:
- *FETCH* latches `imem[pc]` into the instruction register.
- *READ* drives the register file's two read ports (registered, so data lands next cycle). ISETP reads
  its operands from the `prs1/prs2` fields; everything else from `rs1/rs2`.
- *EXEC* runs all 32 lanes through 32 instantiated `alu` modules (or computes special-register values,
  or an ISETP compare), then writes the register file **masked by the guard** (`exec_mask = guard &
  present`) so predicated-off and absent lanes are untouched, and updates the predicate registers or
  finishes on EXIT.

Special registers (`TID_FLAT`, `CTAID_X`, `LANEID`, `NTID_X`, …) are computed per lane from a small
context the host provides (`ctx_bdx/gdx/bidx/tpb`), matching the ISS's `sreg()`. A debug read port
exposes any register's full 32-lane vector (and the predicate registers) after the kernel finishes, so
the testbench can diff the entire final machine state.

**Verification** (`sim/tests/test_cu_core.cpp`): for each kernel it runs the ISS with **final-state
capture** enabled, runs the RTL core on the same `.gpubin` + context, then compares every register on
every lane and predicates P1–P3. Cases: `m3_arith` (ALU coverage), `m3_pred` (ISETP/ISETPI +
`@P`/`@!P` predication), `m3_ids` (special registers with a non-zero **block 2** context), and a
**20-lane partial warp** (proves the `present` mask really masks absent lanes).

**Files added:**
- `rtl/compute/cu_core.sv`, the integrated single-warp core.
- `sim/tests/test_cu_core.cpp`, RTL-vs-ISS testbench.
- `tests/kernels/m3_arith.s`, `m3_pred.s`, `m3_ids.s`, straight-line integer kernels.

**Files modified:**
- `sim/iss/iss.hpp`, added optional **final-state capture** (`cap_enable`, `cap_regs`, `cap_pred`)
  so a chosen warp's ending registers/predicates can be diffed against RTL.
- `sim/run_rtl_tests.sh`, assembles the M3 kernels and adds the `cu_core` build; also gained
  `python3`/`python` auto-detection.
- `Makefile`, `cu_core.sv` added to the lint set.

---

## M4: SIMT control flow: the hardware reconvergence stack ✅

**What's done:** `cu_core.sv` now handles **divergent branches**, the defining SIMT capability. The
core runs `BRA`/`SSY`/`SYNC` with a hardware **reconvergence stack** that mirrors the ISS exactly, and
is checked both against the ISS *and* against known-answer math. 8 core tests pass (4360 checks) in
MSYS2 (5.040) and WSL (5.020): straight-line (M3), divergent if/else, **nested** divergence, and a
**fully-divergent 32-lane loop**.

**How the reconvergence stack works** (in `cu_core.sv`): the warp's state is a stack of frames
`{mask, pc, rpc, pend}` (plus a warp-wide `exited` mask and the predicate regs). The top of stack is
the executing context. The FSM gained a **STEP** phase that, each step, either pops the top frame
(if its lanes are all inactive, or its `pc` reached its reconvergence point `rpc`) or fetches/executes
the next instruction. On a **divergent** `BRA` (some guard-true lanes, some not):
- the current frame becomes the **join** frame, full pre-divergence mask, `pc = R` (the reconvergence
  point named by the preceding `SSY`), keeping its outer `rpc`;
- two child frames are pushed (not-taken then taken), each with `rpc = R`.
A child pops when its `pc` reaches `R`; once both drain, the join frame resumes at `R` with the
reconverged mask. `DONE` is an out-of-range PC sentinel meaning "run to EXIT". This is the classic
IPDOM stack and it handles nesting and loops (a fully-divergent loop legitimately grows the stack by
~one frame per exiting lane, matching the ISS's unbounded stack, hence `STACK_DEPTH = 64`).

**Why it's trustworthy:** the three divergent kernels are checked two ways, the RTL's final registers
must equal the ISS's (`cap_regs`), **and** the ISS's output is checked against a hand-derived answer
(`|lane-16|`, the nested 1000/1101/1201 pattern, and `lane*(lane+1)/2` for the loop). So a bug in the
ISS can't hide behind "RTL matches ISS."

**Files added:**
- `tests/kernels/m4_abs.s`, divergent if/else (abs via a real branch).
- `tests/kernels/m4_nested.s`, nested divergence (outer range split, inner parity split).
- `tests/kernels/m4_loop.s`, per-lane divergent loop (`sum 0..laneid`).

**Files modified:**
- `rtl/compute/cu_core.sv`, evolved from the M3 straight-line core into a stack-based SIMT core:
  added the frame stack, the STEP phase, `BRA`/`SSY`/`SYNC` handling, and the divergence/reconvergence
  logic; raised `STACK_DEPTH` to 64; dropped the M3 "no branches" assertion (memory is still asserted
  absent until M5).
- `sim/tests/test_cu_core.cpp`, added the three M4 cases plus an 8-lane loop variant, and a
  `ground`-truth checker hook so the ISS output is verified against known answers (unbiased).
- `sim/run_rtl_tests.sh`, assembles the M4 kernels.

---

## M5: memory: the core runs real memory kernels ✅

**What's done:** `cu_core.sv` gained a **constant bank** (for `LDC`, where kernel parameters live) and a
**global-memory** subsystem with a load/store unit, so `LDG`/`STG`/`LDC` work. The RTL core now runs the
**M1 memory kernels**, `vec_add` and `saxpy_int`, end-to-end (memory *and* divergence together), and its
final **global memory** is diffed against the ISS *and* against known-answer math. 10 core tests pass
(4721 checks) in MSYS2 (5.040) and WSL (5.020). This is the milestone where the RTL executes the very
programs the ISS validated back in M1.

**How memory works** (in `cu_core.sv`):
- **Constant bank**, a small word-addressed memory the host preloads with kernel parameters (out/a/b
  pointers, `n`, `alpha`). `LDC Rd, c[k]` broadcasts `cbank[k]` to all active lanes (handled inline in EXEC,
  like an ALU op).
- **Global memory**, a word-addressed memory that **persists through reset** (only control state resets),
  so the host preloads inputs once and reads results back through debug ports. `LDG`/`STG` enter a new
  **MEM** phase that walks the active lanes **sequentially** (one word/cycle) against a single-port memory, the un-coalesced worst case, but functionally exact. (A coalescer + L1 cache are performance work; the
  ISA semantics are per-lane independent accesses, so results are already correct.) Loads accumulate into a
  vector that's written back (masked by the guard) when the walk completes; stores write straight to memory.
- The register file's second read port is repurposed for the **store-data** register (`STG` reads the base
  from `rs1` and the data from the `rd` field).

**Multi-warp kernels on a single-warp core:** `vec_add`(n=100) is 4 workgroups. The testbench invokes the
single-warp core once per workgroup over the *shared, persistent* global memory, setting the block context
each time so `TID_FLAT` is right. The kernels' range guard (`@P1 BRA Ldone`) means the last workgroup
**diverges** (some lanes out of range), so this exercises memory and reconvergence at once.

**Files modified:**
- `rtl/compute/cu_core.sv`, added the constant bank, global memory (host preload/readback + core
  store port), the LSU MEM phase, and `LDC`/`LDG`/`STG` handling; the "no memory" assertion is now
  scoped to shared memory only (LDS/STS remain M6).
- `sim/tests/test_cu_core.cpp`, added `run_mem()` and the `vec_add`/`saxpy_int` end-to-end tests
  (global memory vs ISS + known-answer).
- `sim/run_rtl_tests.sh`, also assembles `vec_add`/`saxpy_int` for the RTL suite.

---

## M6: shared memory + barriers ✅

**What's done:** `cu_core.sv` gained per-workgroup **shared memory** (`LDS`/`STS`) reusing the LSU walk, and
`BAR` executes as a barrier. A shared-memory kernel (`m6_shared`: each lane writes `lane²` to `shared[lane]`,
barriers, then reads `shared[31-lane]`) passes against the ISS and known-answer math. 11 core tests, 5269
checks, both environments.

**How it works:** shared memory is a second word-addressed memory in the core; the MEM phase routes to it
(instead of global) for `LDS`/`STS` based on the opcode. `BAR` is a no-op for a single resident warp
(a real cross-warp barrier arrives with the multi-warp scheduler, see below). Writing this milestone
surfaced a real bug the **unbiased test caught**: the store-data register mux only handled global `STG`, so
`STS` read the wrong register, fixed to cover both. (This is exactly why the tests check against
known answers, not just RTL-vs-ISS.)

**Scope note:** atomics and a true cross-warp barrier need *multiple resident warps* executing
concurrently, which this single-warp core doesn't have yet (the testbench runs warps one at a time over
shared global memory). Multiple resident warps + a scheduler + atomics are deferred to a later hardware
pass; they are not on the critical path to the project's completion goal (compiling high-level kernels).

**Files added:** `tests/kernels/m6_shared.s`.
**Files modified:**
- `rtl/compute/cu_core.sv`, added the shared-memory array and `LDS`/`STS` routing in the MEM phase;
  fixed the store-data register selection to cover shared stores; removed the now-satisfied "no shared
  memory" assertion.
- `sim/tests/test_cu_core.cpp`, set `shared_words` on the ISS and added the `m6_shared` test.
- `sim/run_rtl_tests.sh`, assembles `m6_shared`.

---

## The compiler, high-level kernels → assembly → binary ✅ (completion goal)

**What's done, the project's goal is reached:** you can now write a kernel in a **C-like language**, and
`scripts/sirc.py` compiles it to **Sirion assembly**, which `scripts/asm.py` assembles to a **`.gpubin`**,
which runs correctly on **both** the golden ISS and the **RTL GPU core**. Three high-level kernels
(`vec_add`, `saxpy`, `strided_sum`) pass end-to-end (217 ISS checks vs known answers), and the compiled
`vec_add` also runs on the hardware core (`cu_core_hl_vec_add`), all green in MSYS2 and WSL.

**The language** (`examples/kernels/*.k`) is an integer subset matching what the ISA/ISS/RTL support:
`kernel` functions with `int`/`int*` parameters; `int` locals; `if/else`, `while`, `for`; assignment;
array load/store `p[e]`; arithmetic `+ - *`, bitwise `& | ^ ~ << >>`, unary `- ~`; comparisons in
conditions; and the builtins `tid() lane() ctaid() ntid() nctaid()`. For example:
```c
kernel vec_add(int* out, int* a, int* b, int n) {
    int i = tid();
    if (i < n) { out[i] = a[i] + b[i]; }
}
```

**The compiler** (`scripts/sirc.py`) is a real, if deliberately small, compiler:
1. **Lexer** → tokens; **recursive-descent parser** (with precedence climbing for expressions) → an AST.
2. **Code generation** by tree-walk, with a **linear register allocator**: each variable holds one of the
   16 registers for its lifetime; expression temporaries come from a free pool (no spilling yet, a kernel
   needing >16 live values is a clean compile error). Kernel parameters are read from the **constant bank**
   in declaration order (`out`→`c[0]`, …), matching the launch convention.
3. **Control-flow lowering:** `if/else`, `while`, and `for` are lowered to the ISA's SIMT divergence
   pattern, a predicate set with `ISETP`, an `SSY` naming the reconvergence point, and a guarded
   `@!P BRA`, so the compiled code drives the **same hardware reconvergence stack** built in M4. Array
   accesses lower to address arithmetic (`idx*4 + base`) plus `LDG`/`STG`.

This is why the pieces line up: the compiler emits the exact ISA the assembler encodes, the ISS executes,
and the RTL core runs, one contract, four implementations, all verified against each other and against
known answers.

**Files added:**
- `scripts/sirc.py`, the C-like → Sirion-assembly compiler.
- `examples/kernels/vec_add.k`, `saxpy.k`, `strided_sum.k`, high-level kernels.
- `sim/tests/test_hl.cpp`, runs the compiled kernels on the ISS vs known answers.
- `sim/run_hl_tests.sh`, compiles the `.k` kernels and runs the HL tests.

**Files modified:**
- `run_all.sh`, added the high-level compiler test stage.
- `Makefile`, added the `hl` target.
- `sim/run_rtl_tests.sh`, compiles the high-level `vec_add` for the RTL end-to-end check.
- `sim/tests/test_cu_core.cpp`, added `cu_core_hl_vec_add` (compiled kernel on the hardware core).

---

## M7: memory hierarchy + scheduler (performance) ✅

**What's done:** the two performance building blocks the roadmap calls out, a **cache** and a **warp
scheduler**, as standalone, verified modules.

- **L1 data cache** (`rtl/cache/l1_cache.sv`), direct-mapped, **write-back, write-allocate**. Keeps
  recently-used lines on-chip so repeated accesses hit instead of going to slow memory. Verified for
  **transparency**: a testbench models a flat golden memory + a line-granular backing store and issues
  8000 random reads/writes over a range large enough to force evictions and write-backs; every read must
  return the golden value (5069 checks). It also checks the hit/miss counters (2718 hits / 6306 misses on
  the random trace). Protocol: pulse `req_valid`; on a miss it writes back the dirty line, fetches the new
  line, then serves, all behind a `resp_ready` handshake.
- **Round-robin warp scheduler** (`rtl/scheduler/warp_sched.sv`), picks one ready warp per cycle, rotating
  fairly so none is starved (this is how a multi-warp CU hides latency: issue from another ready warp while
  one stalls). Verified against a C++ golden over 5000 random ready-masks/advances, plus a fairness test
  (all warps ready + advancing → every warp is served). 9986 checks.

**Integration note:** these are verified as independent modules. Wiring them into `cu_core` (multiple
resident warps sharing the scheduler, loads going through the cache) is a further integration step; the
single-warp core remains the functional path today. Building and proving the components first is the same
bottom-up method used for the M2 datapath blocks.

**Files added:** `rtl/cache/l1_cache.sv`, `rtl/scheduler/warp_sched.sv`,
`sim/tests/test_l1_cache.cpp`, `sim/tests/test_warp_sched.cpp`.
**Files modified:** `sim/run_rtl_tests.sh` (runs both), `Makefile` (lint).

---

## M8: floating point ✅

**What's done:** a single-precision **floating-point ALU** (`rtl/compute/fp_alu.sv`) with **FADD, FMUL, and
FMA** (binary32, round-toward-zero, flush-to-zero denormals). Verified three ways (60008 checks): directed
**known-answer** results on exactly-representable values (`1.0+2.0=3.0`, `2.0*3.0=6.0`, `2*3+1=7`, …) that
must match real IEEE arithmetic; a C++ golden that mirrors the RTL's exact integer algorithm, checked
**bit-for-bit** over 20000 random finite normals; and a check that FMUL lands **within 1 ULP** of the
host's hardware IEEE multiply. FP unblocks moving the graphics vertex stage (transform/projection) onto the
compute core and FP compute kernels; the integer graphics path already renders without it.

**Files added:** `rtl/compute/fp_alu.sv`, `sim/tests/test_fp_alu.cpp`.
**Files modified:** `sim/run_rtl_tests.sh`, `Makefile` (lint).

---

## M9: the graphics pipeline: rendering on the GPU ✅

**What's done:** the GPU now **renders 3D graphics**. A fixed-function **triangle rasterizer** in RTL
(`rtl/graphics/raster.sv`) does triangle setup, edge-equation coverage, barycentric **Gouraud**
interpolation, a **depth test**, back-face culling, and framebuffer writes, and renders a shaded 3D cube
to an image, verified **pixel-for-pixel** against a C++ golden rasterizer in MSYS2 (5.040) and WSL (5.020).
The result is written as a PPM and can be previewed right in the terminal (`scripts/ppm_tool.py`, ANSI
truecolor / ASCII) or exported to PNG.

**How it works:**
- **Vertex stage (host):** `sim/gfx/scene.hpp` rotates + perspective-projects a colored cube and snaps
  vertices to integer screen coordinates, integer depth, and 8-bit colors. (This is the vertex-shader role;
  it will move onto the compute core when the core gets floating point.)
- **Rasterizer (RTL):** for each triangle, `raster.sv` computes the bounding box and the signed area, then
  walks candidate pixels one per cycle. Three **edge functions** (signed 64-bit) give the barycentric
  weights; a pixel is covered when they share the area's sign. Depth and color are interpolated as
  `(w0·a0 + w1·a1 + w2·a2) / area` with integer division, the **exact same integer ops** as the golden
  model, so they agree bit-for-bit. Covered pixels that pass the **depth test** write the color + depth
  buffers (on-chip here; the host clears them and reads color back).
- **Golden + visualization:** `sim/gfx/gfx.hpp` is the reference rasterizer + framebuffer + PPM writer;
  `scripts/ppm_tool.py` previews/exports.

**A real bug the pixel-exact test caught:** the projection can put a vertex slightly *nearer* than the near
plane (negative depth). The golden compares depth **signed** and draws it; the RTL initially compared the
32-bit depth slice **unsigned** (a Verilator part-select is unsigned), so negative depths looked huge and
those fragments were wrongly dropped, 84 missing pixels. Fixed by reinterpreting the depth as signed. This
is exactly why the test demands pixel-exact agreement, not "looks about right."

**Files added:**
- `rtl/graphics/raster.sv`, the RTL triangle rasterizer.
- `sim/gfx/gfx.hpp`, golden rasterizer + framebuffer + PPM writer.
- `sim/gfx/scene.hpp`, the host vertex stage (cube transform + projection).
- `sim/tests/render_demo.cpp`, software render (→ `build/render.ppm`, `make render`).
- `sim/tests/test_raster.cpp`, RTL-vs-golden pixel-exact check + renders `build/render_rtl.ppm`.
- `scripts/ppm_tool.py`, terminal preview (ANSI/ASCII) + PNG export.

**Files modified:**
- `sim/run_rtl_tests.sh`, runs the rasterizer test.
- `Makefile`, added `raster.sv` to lint and a `make render` target.

---

## M10: textures, host API, performance counters ✅

**What's done:** the graphics stack is finished off with texturing, a clean host API, and perf counters, and it renders a **texture-mapped 3D cube on the RTL GPU**.

- **Texture unit**, `raster.sv` gained a texture memory, per-vertex **UV** interpolation (same barycentric
  integer math as color/depth), and **nearest** sampling with clamp addressing. The cube is UV-mapped per
  face and rendered with a checkerboard texture; the RTL output matches the golden model pixel-for-pixel
  (`build/render_tex_rtl.ppm`). `sim/gfx/gfx.hpp` gained the matching `Texture` + sampling.
- **Host graphics API** (`sim/hostapi/gpu.hpp`), a small C++ facade in the classic style:
  `gpu.initialize(...)`, `gpu.createTexture(...)`, `gpu.drawMesh(...)`, `gpu.present("out.ppm")`, plus
  triangle/fragment counters. `examples/render_scene.cpp` uses it to render a textured cube (`make`-able).
- **Performance counters**, `raster.sv` exposes `perf_pixels` (candidate pixels tested) and `perf_frags`
  (fragments that passed the depth test), cleared with the framebuffer. (The ISS already tracks
  `dyn_insns`, `lane_ops`, `divergent_branches`; the cache and scheduler expose hit/miss and selection.)
- **Visualization**, `scripts/ppm_tool.py` previews any render in the terminal (ANSI truecolor or ASCII)
  and exports PNG. `make render` builds the software demo and previews it.

**A real bug the pixel-exact test caught** (M9, worth repeating): a vertex projecting slightly nearer than
the near plane gives a *negative* depth; the RTL compared the depth slice **unsigned**, dropping those
fragments, while the golden compared **signed**. Fixed by reinterpreting the depth as signed. Known-answer
pixel-exact testing is what surfaced it.

**Files added:** `sim/hostapi/gpu.hpp`, `examples/render_scene.cpp`.
**Files modified:** `rtl/graphics/raster.sv` (texture unit + perf counters), `sim/gfx/gfx.hpp` +
`sim/gfx/scene.hpp` (texture + UVs), `sim/tests/test_raster.cpp` (Gouraud + textured passes).

---

## M11: multi-warp compute unit + integrated scheduler + real barrier ✅

**What's done:** `cu_core` is no longer a single warp, it now holds **NUM_WARPS resident warps** (default 8,
so up to 256 threads/block), arbitrates between them with the integrated round-robin **`warp_sched`**, and
implements a **real cross-warp `BAR` barrier**. This is the first step of the staged evolution to a real
multi-warp GPU (roadmap M11–M15).

**Execution model, interleaved multithreading.** One warp advances through the multicycle engine at a time.
The scheduler picks a *ready* warp and runs it until it **yields**: either it finishes (its reconvergence
stack empties) or it hits `BAR` and **parks**. When no warp is ready but some are parked, the barrier is
**released** (all parked warps become ready). `ready[w] = resident[w] & !finished[w] & !at_barrier[w]`; the
block is done when no warp is ready and none is parked. A new FSM state `S_SCHED` does the picking/releasing.

- **Per-warp state** (`cu_core.sv`), the reconvergence `stack` + `sp`, `exited` mask, predicate regs
  `pred1..3`, and new `finished`/`at_barrier`/`resident` bits became **arrays indexed by warp id**; a
  `cur_warp` register tracks who's executing. The instruction register, memory-lane walker (`mlane`) and
  load accumulator (`ld_vec`) stay single, only one warp runs at a time and a memory op completes before any
  yield. Per-warp `present`/residency are derived from `ctx_tpb` (warp `w` owns lanes `w*32+lane < tpb`), so
  the old separate `present` port is gone.
- **Register file** (`regfile.sv`), storage became `regs[NUM_WARPS][NUM_VREGS]`, selected by a new `warp`
  port. Reads and writes share one selector: normal accesses use `cur_warp`; the debug read uses `dbg_warp`
  while writes are quiesced. One read/write port-pair still suffices (one warp executes at a time).
- **Real `BAR`**, on `OP_BAR`, the warp sets `at_barrier`, advances past the BAR, and yields to `S_SCHED`;
  it cannot be re-picked until the whole block reaches the barrier and it is released. Special registers
  (`RDSR`) are now per-warp: thread-in-block `= cur_warp*32 + lane`, so `TID_X`/`WARPID`/`TID_FLAT` are
  correct for every warp (previously hard-wired to warp 0).
- **Golden model** (`iss.hpp`), `run_block` was rewritten to **cooperative stepping** with persistent
  per-warp state (`WarpState`): step a warp to its next `BAR`/exit, release when all live warps are parked.
  Before this, `BAR` was a no-op and warps ran serially to completion, which is **wrong** for a kernel that
  communicates across warps through shared memory around a barrier. Capture was extended to record **every**
  warp of the captured block (`cap_warps[]`), so the test can diff all warps.

**A real barrier, proven (unbiased).** The new kernel `tests/kernels/block_shuffle.s` has every thread write
its global id to `shared[tid_local]`, `BAR`, then read its **mirror** slot `shared[tpb-1-tid_local]` (written
by a thread in a *different* warp) and store it to `out[gid]`. Because **every** thread reads another warp's
write, the closed-form answer (`out[b*tpb+t] = b*tpb + (tpb-1-t)`) only holds if the barrier truly
synchronizes the warps. `test_cu_core.cpp` runs it at blockDim 64 / 256 / 100 (2, 8, and 4-warp-with-partial)
and checks **every output** against the known answer **and** **every warp's** final registers bit-exact vs
the ISS. Temporarily replacing the hardware barrier with a no-op makes all three fail (RTL reads `0` for
not-yet-written mirror slots), confirming the barrier is load-bearing, not cosmetic. SVA was added: an
executing warp is never parked, and no warp is ever both finished and parked.

**Files added:** `tests/kernels/block_shuffle.s`.
**Files modified:** `rtl/compute/cu_core.sv` (multi-warp state + `warp_sched` + `BAR`), `rtl/compute/regfile.sv`
(per-warp `warp` port), `sim/iss/iss.hpp` (cooperative barrier + all-warp capture), `sim/tests/test_cu_core.cpp`
(multi-warp/barrier harness + 3 cases; drop the removed `present` port, add `dbg_warp`), `sim/run_rtl_tests.sh`
(assemble `block_shuffle`; add `warp_sched.sv` to the `cu_core` build). Test totals for the `cu_core` group
rose from 12 cases / 5,473 checks to **15 cases / 15,363 checks**.

**Next (M12):** hardware **block/grid dispatch**, a small command processor that iterates the grid and
launches blocks onto the CU (replacing the C++ per-block loop in the testbench), setting per-warp CTAID/TID.

---

## M12: hardware block/grid dispatch ✅

**What's done:** grid execution moved from the C++ testbench into hardware. A **workgroup dispatcher**
(`rtl/scheduler/cta_dispatch.sv`) latches a launch descriptor (grid size, threads/block, entry PC) and walks
the grid, launching each block onto the CU (driving `start`/`ctx_bidx`, waiting for `done`) until the whole
grid completes, then pulses `grid_done`. A new top level **`rtl/gpu_top.sv`** is the host-facing device:
preload program/constants/memory, write the descriptor, pulse `launch`, wait, the hardware does the rest.
Back-to-back relaunches on the same device work without reset. `perf_blocks` counts dispatched blocks.

**Tests** (`sim/tests/test_gpu_top.cpp`): `vec_add` over a 4-block grid, the barrier `block_shuffle` at
blockDim 64 and 100 (partial warp), and a relaunch case, each a **single `launch`**, diffed against the
ISS and closed-form answers, including every warp's final registers.

**Files added:** `rtl/scheduler/cta_dispatch.sv`, `rtl/gpu_top.sv`, `sim/tests/test_gpu_top.cpp`.
**Files modified:** `sim/run_rtl_tests.sh` (gpu_top group), `Makefile` (lint list).

---

## M13: the barrel pipeline (~4x throughput) ✅

**What's done:** the multicycle FSM (~5 cycles/instruction, one warp at a time) was re-microarchitected into
a 4-stage **barrel pipeline**, the classic GPU fine-grained-multithreading design:

```
PICK ──────► FETCH ─────► READ ──────► EXEC ─────► (memory unit, in parallel)
pick ready   imem read    regfile      ALU/branch/  lane-walking LDG/STG/LDS/STS runs
warp; pop    (1 cycle)    read         writeback    WHILE other warps keep executing
dead frames               (1 cycle)    (1 cycle)
```

- **Each stage holds a different warp**, and at most **one instruction per warp** is in flight
  (`in_flight[w]`), so intra-warp RAW/WAW hazards are impossible by construction, the per-warp in-flight
  bit *is* the scoreboard. With ≥4 ready warps the core issues ~1 instruction/cycle.
- **The memory unit detached from the pipeline:** a LDG/STG/LDS/STS is handed off at EXEC and walks only the
  **active** lanes (priority-encoded, so divergent memory ops cost #active-lanes cycles, not 32) while the
  pipeline keeps issuing other warps, the first real **latency hiding**. It has its **own register-file
  write port** (`regfile.sv` gained separate read/write warp selectors + a second write port), so a load
  writeback never collides with an ALU writeback. A second memory op reaching EXEC while the unit is busy
  stalls the pipe (structural hazard, handled).
- **Architecture unchanged:** reconvergence stacks, predication, BAR park/release, special registers, all
  M11 semantics preserved; the ISS needed **no changes** and the RTL still matches it bit-exact.
- **Perf counters added** (`perf_cycles`, `perf_insns`, `perf_memops`) and surfaced through `gpu_top`.

**Measured:** compute-bound 8-warp divergent-loop kernel: **IPC 0.756** vs ~0.2 for the multicycle core
(~3.8×), the gap to 1.0 is reconvergence-stack pops consuming PICK slots and the loop tail draining warps.
Memory-bound kernels remain limited by the serial lane walk, exactly what M14's coalescer + cache address.

**Tests:** all 15 `cu_core` cases (M3–M11) pass **unchanged** on the new microarchitecture, the strongest
evidence the pipeline is architecturally transparent, plus a new `gpu_top_pipeline_ipc` test that checks
every warp's registers vs the ISS *and* asserts IPC > 0.5. New SVA: an executing/memory warp is always
in-flight; ready and in-flight are mutually exclusive; the RF write ports never target the same warp.

**Files modified:** `rtl/compute/cu_core.sv` (rewritten around the pipeline), `rtl/compute/regfile.sv`
(rwarp/wwarp + write port 2), `rtl/gpu_top.sv` (perf passthrough), `sim/tests/test_gpu_top.cpp` (IPC test).

---

## M14: memory coalescer + L1 data cache ✅

**What's done:** global memory accesses now go through a real GPU memory path. The memory unit gained a
**coalescer**: the active lanes of an LDG/STG are grouped by cache line (leader lane's line, matched
combinationally across all 32 lanes) and each group becomes **one line transaction** on a new
**line-granular L1 D-cache**, a fully-coalesced 32-lane access is 32/LINE_WORDS = **4 transactions instead
of 32 serial walks**. Store groups assemble a per-word write-strobe line (highest lane wins per word,
matching ISS lane order); load responses scatter the line back to every matched lane at once.

- **`rtl/cache/l1_dcache.sv`** generalizes the M7 word-granular cache to line transactions with per-word
  strobes: direct-mapped, write-back, write-allocate; hit = 2 cycles, clean miss = 4, dirty miss = 5; plus a
  **flush** FSM (write back all dirty lines + invalidate) used before host readback and between launches.
- **Coherency seam:** `cta_dispatch` auto-flushes the L1 when a grid completes, so `grid_done` guarantees a
  coherent backing memory, host code (and the `gpu_top` tests) need no cache knowledge at all.
  `cu_core`-level tests flush explicitly before reading `gmem`.
- **Shared memory stays a scratchpad** (serial active-lane walk), as in real GPUs, LDS/STS never touch L1.
- **Two real bugs caught by the unit test** (`sim/tests/test_l1_dcache.cpp`, 665k checks vs a flat-memory
  golden): the registered flush write pulse landed one cycle after the set counter advanced (wrote the wrong
  line), and the backing-address mux selected the flush path in the wrong state. Both fixed; the random
  transparency test (reads always equal flat memory; post-flush backing equals the golden image) now passes.

**Measured** (`test_gpu_top.cpp`): `vec_add` with all lanes active: **exactly 4.0 line transactions per
memory op** (perfect coalescing); `loop_reduce` (per-lane sequential walk → line reuse): **86.8% L1 hit
rate**. All 15 `cu_core` cases + 7 `gpu_top` cases remain bit-exact vs the ISS (cache transparency).

**Files added:** `rtl/cache/l1_dcache.sv`, `sim/tests/test_l1_dcache.cpp`.
**Files modified:** `rtl/compute/cu_core.sv` (coalescer + L1 in the memory unit; flush port; L1 perf
counters), `rtl/scheduler/cta_dispatch.sv` (auto-flush at grid end), `rtl/gpu_top.sv` (wiring + counters),
`sim/tests/test_cu_core.cpp` (flush before readback), `sim/tests/test_gpu_top.cpp` (coalescing + hit-rate
tests), `sim/run_rtl_tests.sh`, `Makefile`.

---

## M15: floating point in the execute stage ✅

**What's done:** the GPU computes in binary32 floats. Nine FP opcodes joined the ISA (0x35–0x3D):
`FADD FSUB FMUL FFMA FMIN FMAX I2F F2I FSETP`, kept in lockstep across `sirion_pkg.sv`, `isa.hpp`, and
`asm.py`. **32 `fp_alu` lanes** sit beside the integer lanes in EXEC; `FSETP` writes predicate registers
using a float total-order compare, so `@P`-guarded float control flow works.

- **`fp_alu.sv` grew** from 3 ops to 7 (op widened to 3 bits): FMIN/FMAX (total-order on FTZ-canonicalized
  keys), I2F (int32→float, truncate) and F2I (float→int32, truncate + saturate). FSUB = FADD with the
  addend's sign flipped. Semantics: **round-toward-zero, flush-to-zero denormals, Inf/NaN out of scope**, deterministic and bit-exactly mirrored by `fp_add/mul/fma/min/max/i2f/f2i/cmp_ref` in `isa.hpp`, which are
  now shared by the ISS (execution), the fp_alu unit test, AND the RTL, one golden, three consumers.
- **`FFMA rd, rs1, rs2` computes rd = rs1·rs2 + rd**, the accumulator is the destination register, read
  through a new **third register-file read port** (`regfile.sv`), so a fused multiply-add issues like any
  other instruction (no extra cycles, no encoding change).
- **Compiler `float` support is deferred to M21** (compiler-completeness milestone), M15 FP is exercised
  from assembly.

**Tests:** `fp_alu` 4/4 (140k checks: directed IEEE + random vs golden + new min/max/convert sweep +
1-ULP FMUL bound). New kernel `tests/kernels/saxpy_float.s` (`out[i] = a·x[i] + y[i]` via FFMA) runs
through the **whole pipeline** (FP loads → FFMA on the 3rd read port → FP store through the L1) with
exactly-representable values so RTZ == IEEE: the ISS is checked against **real float arithmetic** and the
RTL against the ISS **bit-exact**. cu_core: 16 cases, all green.

**Files modified:** `sim/iss/isa.hpp` (FP opcodes + golden refs + decode_ref), `sim/iss/iss.hpp` (FP
execution), `rtl/sirion_pkg.sv`, `rtl/compute/decode.sv` (falu class + FSETP), `rtl/compute/fp_alu.sv`
(4 new ops), `rtl/compute/regfile.sv` (read port 2), `rtl/compute/cu_core.sv` (FP lanes + FSETP + port
wiring), `scripts/asm.py` (FP mnemonics + FSETP), `sim/tests/test_fp_alu.cpp`, `sim/tests/test_cu_core.cpp`
(saxpy_float), `sim/run_rtl_tests.sh`.
**Files added:** `tests/kernels/saxpy_float.s`.

---

## M16: atomics + special-function unit ✅

**What's done:** the two features that make real GPGPU kernels possible, **atomic read-modify-write**
(lock-free reductions/histograms) and a **special-function unit** (transcendentals).

- **Atomics** (`ATOMG.fn` / `ATOMS.fn`, fn ∈ ADD/MIN/MAX/EXCH/CAS, opcode 0x3E/0x3F, func in insn[10:8]):
  `rd` receives the OLD value; CAS's compare value is `rd` going in (read via the M15 third RF port).
  Lanes commit in ascending lane order (defines same-address semantics, matching the ISS). **Global
  atomics execute INSIDE the L1 D-cache**, a new atomic word-transaction type RMWs the cached line
  directly (hit: 2 cycles; miss: fill then RMW), which is what keeps cached data coherent; they
  deliberately never coalesce (one transaction per lane). **Shared-memory atomics** RMW the scratchpad
  during the serial lane walk. The dispatcher's grid-end flush makes results host-visible as usual.
- **SFU** (`rtl/compute/sfu.sv`, `MUFU.fn rd, rs1`, fn ∈ RCP/RSQRT/SIN/COS, opcode 0x2D): RCP is an
  **exact truncated integer division** (2^46/f, ≤1 ULP); RSQRT is a truncated integer square root
  (non-restoring, 24 iterations) followed by RCP (≤~2 ULP); SIN/COS use a **quarter-wave 65-entry Q15
  LUT + linear interpolation** over a Q16 fraction-of-turn phase (~1e-4 absolute, ±8 rad tested; real
  GPUs' MUFU is likewise approximate). All four are integer algorithms mirrored **bit-exactly** by
  `sfu_*_ref` in `isa.hpp`, the ISS executes the same code, so RTL == ISS exactly even though the
  functions are approximations of the real math. One SFU per lane, issued like any ALU op.
- **A real bug the unit test caught:** the LUT interpolation index was 6 bits, so `LUT[63+1]` wrapped to
  `LUT[0]`, sin values just below 90° interpolated toward 0 and overflowed Q15 (results > 1.0). The
  bit-exact-vs-golden sweep caught it immediately; index widened to 7 bits.

**Tests:** `sfu` unit test (100k checks: bit-exact vs golden + libm accuracy bounds); gpu_top
**multi-block atomic histogram**, 1000 threads over 4 blocks incrementing 16 bins concurrently, matches
the closed-form counts exactly (a non-atomic RMW would lose updates); `rsqrt_map` kernel through the whole
pipeline, bit-exact vs ISS and within tolerance of `1/sqrtf`. All prior suites stay green (cu_core 16,
gpu_top 9, l1_dcache transparency with atomics added).

**Files added:** `rtl/compute/sfu.sv`, `sim/tests/test_sfu.cpp`, `tests/kernels/histogram.s`,
`tests/kernels/rsqrt_map.s`.
**Files modified:** `sim/iss/isa.hpp` (opcodes, `atom_ref`, `sfu_*_ref`, decode), `sim/iss/iss.hpp`
(MUFU + atomic execution), `rtl/sirion_pkg.sv`, `rtl/compute/decode.sv`, `rtl/cache/l1_dcache.sv`
(atomic transactions), `rtl/compute/cu_core.sv` (SFU lanes + atomic dispatch/walk), `scripts/asm.py`,
`sim/tests/test_gpu_top.cpp`, `sim/run_rtl_tests.sh`, `Makefile`.

---

## M17: multiple compute units + shared L2 ✅

**What's done:** Sirion scales out. `gpu_top` now instantiates **NUM_CU compute units**, each with a private
L1, sharing one **write-back L2 cache** (round-robin arbitrated) in front of global memory; the dispatcher
hands the blocks of one launch to **whichever CU is free**, so blocks genuinely execute concurrently.

```
 launch ─► CTA DISPATCH ──► CU0 ─ L1 (write-through) ─┐
           (block -> any   ├──► CU1 ─ L1 ─────────────┤─► L2 (write-back, RR arbiter,
            free CU)       └──► CU…                   ┘    atomics) ─► GLOBAL MEMORY
```

- **The L1 became write-through / no-write-allocate** (M14 built it write-back). This is forced by
  correctness, not preference: private write-back L1s without coherence lose data to **false sharing**, two CUs writing different words of the same line would each evict a whole stale line. Write-through
  stores forward per-word strobes to the L2, which merges them; loads still allocate and hit locally.
  This is the same policy real NVIDIA GPUs use for global memory.
- **Atomics moved from L1 to L2**, the L2 is the single serialization/coherence point for the whole GPU
  (an atomic self-invalidates the L1's copy of the line). Cross-CU visibility contract: guaranteed at
  kernel boundaries and through atomics, CUDA's model.
- **Global memory moved out of `cu_core`** into `gpu_top`; the CU exposes its L1's memory side as a
  **level-handshake port** (hold req until ack, required because the shared L2's service latency varies
  with contention). `l2_cache.sv` serves NUM_CU ports round-robin: line reads (L1 fills), strobed line
  writes, atomic word RMWs; write-back + write-allocate with a flush walker.
- **Grid-end coherence sequence:** dispatcher waits for all CUs to drain, then invalidates every L1 and
  flushes the L2, `grid_done` *means* host-coherent memory.
- **The ISS needed no changes** (it executes blocks serially, which is a valid reference for race-free
  kernels, and the memory model above makes block-disjoint word writes race-free by construction).
- **Test infrastructure consolidated:** all kernel tests now drive `gpu_top` through a shared helper
  (`sim/tests/gpu_tb.hpp`); the kernel-correctness suite builds it with 1 CU (registers diffed vs ISS
  exactly), and a new `test_multi_cu.cpp` builds it with `-GNUM_CU=4`.

**Tests (4-CU build):** vec_add (16 blocks), the cross-warp barrier shuffle (8 blocks), FP saxpy
(10 blocks), and a **2000-thread histogram hammering 16 bins from all 4 CUs at once** (atomics serialize
at the L2; counts exact), plus a **scaling check**: measured **2.93× speedup on 4 CUs** vs the
serial-lower-bound (wall cycles vs total instructions retired). All prior suites green: kernel suite
16/16 (15,892 checks), system suite 9/9 (coalescing still exactly 4.0 lines per load; 87.5% L1 hit rate).

**Three real bugs the tests caught bringing this up:**
1. the L1 line-aligned the atomic's address on its memory side, dropping the word offset (write-through
   unit test caught it);
2. the L2 arbiter's round-robin index wrapped modulo a power of two, so with NUM_PORTS=1 it scanned an
   out-of-range bit and **never picked any request**, the first global load hung the GPU (a probe
   testbench with perf-counter progress prints localized it in minutes);
3. a **level-handshake race**: the L2 re-sampled a port's `req` in the same cycle its `ack` was visible,
   re-executing the latched request, invisible on idempotent loads/stores, but the **atomic histogram
   counted exactly double**. Fix: `mem_req` deasserts combinationally on `ack`. This is why the milestone
   plan insists on non-idempotent (atomic) tests, not just load/store transparency.

**Files added:** `rtl/cache/l2_cache.sv`, `sim/tests/gpu_tb.hpp`, `sim/tests/test_multi_cu.cpp`.
**Files modified:** `rtl/cache/l1_dcache.sv` (write-through rework + req/ack memory side),
`rtl/compute/cu_core.sv` (global memory removed; L1 memory port exported), `rtl/gpu_top.sv` (CU array +
L2 + global memory + dbg_cu), `rtl/scheduler/cta_dispatch.sv` (multi-CU dispatch + drain + flush
sequencing), `sim/tests/test_cu_core.cpp` + `test_gpu_top.cpp` (rebased on gpu_tb.hpp),
`sim/tests/test_l1_dcache.cpp` (write-through golden), `sim/run_rtl_tests.sh` (explicit parallel make;
multi_cu group), `Makefile`.

---

## M18: advanced fixed-function graphics ✅

**What's done:** the rasterizer grew the features that separate a toy from a real raster pipe, all in
integer math mirrored bit-for-bit by the golden model (`sim/gfx/gfx.hpp`), still pixel-exact.

- **Perspective-correct interpolation.** Each vertex carries `q = 1/w` in Q16; attributes interpolate as
  `attr = Σ(wᵢ·aᵢ·qᵢ) / Σ(wᵢ·qᵢ)`. The elegance: with equal `q` this reduces **exactly** to the old
  affine `Σ(wᵢ·aᵢ)/area` (barycentric identity `w0+w1+w2 = area`), so every M9/M10 scene renders
  unchanged and no separate affine path exists. Depth stays screen-linear (standard Z-buffer).
- **Bilinear filtering**: 4 clamped texels, half-texel center offset, Q8 weights, `>>16` blend.
- **Mipmaps**: the texture memory holds a box-filtered **pyramid**; the per-triangle LOD comes from the
  texel-area/pixel-area ratio (each level covers 4× the area), the standard constant-LOD shortcut.
- **ROP stage**: depth test + optional **alpha blend** `C = (Cs·a + Cd·(255−a))/255` + optional depth-write
  (off for translucent passes). Kept inside `raster.sv` for now; it factors out as a standalone module in
  M20 when the programmable fragment path needs to feed it directly.
- **Windows macro strikes again**: `DrawState` is a `wingdi.h` macro (`DrawStateA`), the golden model's
  draw-state struct is `RasterState` (joins `far`, `inside`, `OUT`, `IN` on the rename list).

**Tests** (`test_raster.cpp`, all pixel-exact RTL vs golden): the M9/M10 cube scenes unchanged; a
perspective floor quad that provably **differs from the affine render** (>1000 px, the correction is
live, not cosmetic); a magnified bilinear triangle that differs from nearest; a minified 1-texel checker
that mip-selects to mid-gray (closed-form); and a translucent triangle whose overlap pixel equals the
closed-form blend exactly, with depth-write disabled.

**Files modified:** `rtl/graphics/raster.sv` (q ports, perspective interpolation, pyramid texmem, bilinear,
LOD, blend/depth-write), `sim/gfx/gfx.hpp` (`Vtx.q`, `Texture::build_mips/sample_bilinear/sample_lvl`,
`RasterState`, `raster_triangle_ex`; old `raster_triangle` is now a compatibility wrapper),
`sim/tests/test_raster.cpp` (harness + 4 new feature tests).

---

## M19: programmable shaders on the compute unit ✅

**What's done:** the defining feature of a modern GPU, **vertex and fragment shaders are just kernels**
that run on the same SIMT compute units as compute work. One test drives the entire programmable flow on
one DUT (`rtl/gpu_gfx_top.sv` = the compute GPU + the rasterizer):

1. **Vertex shader** (`tests/kernels/vs_persp.s`) runs on the CU: FP transform (`FADD`/`FMUL`), the
   perspective divide via **`MUFU.RCP`**, `F2I` snap, and the Q16 `1/w` for M18's perspective-correct
   interpolation, model-space vertices in, screen-space vertices out, all through global memory.
2. **Rasterizer fragment-EMIT mode**: coverage + perspective-correct varying interpolation, but instead of
   shading, each covered pixel becomes a **fragment record** ({z,pidx},{v,u},{rgb}) in a fragment buffer
   (late-Z; the depth test moves to the ROP).
3. **Fragment shader** (`tests/kernels/fs_texmod.s`) runs on the CU over the fragments: samples the texture
   **with ordinary global loads** (no TEX instruction needed, the GPGPU machine *is* the texture unit;
   the M18 fixed-function texunit remains for the FF path), modulates by the interpolated color, shades
   the records in place.
4. **ROP**: the rasterizer's new fragment-input port consumes the shaded fragments, depth test, alpha
   blend (the second draw is translucent at α=160 with depth-write off), framebuffer write.

The host moves the small buffers between engines in M19 (M20's command processor + raster memory port
automate that). **Verification:** every stage is diffed against a stage-exact golden built from the *same*
shared reference functions the ISS executes (`fp_*_ref`, `sfu_rcp_ref`), VS output bit-exact, all 3357
fragment records bit-exact, FS colors bit-exact, and the final framebuffer **pixel-exact** (13,480 checks).
The output image `build/render_shaded_rtl.ppm` was rendered by shader programs executing on the Sirion GPU.

**A real bug the pipeline caught:** interpolated `u/v` reach exactly 32.0 texels on quad edges; the FS
sampled one texel past the texture (the RTL read zeros, black fringe). Real samplers clamp: the FS gained
a `MIN`-clamp (and the golden the same), a nice example of why shader code must handle edge coordinates.

**Programmer's guide + CLI runners added** (usable for any kernel, not just shaders):
`docs/PROGRAMMING.md` is the hands-on write→compile→run guide, and `tools/sirion_run.cpp` builds two
runners, `make runner` (golden ISS, instant) and `make runner-rtl` (**the real RTL GPU** with live
cycle/IPC/cache counters), so programs run from the command line with no C++ at all.

**Files added:** `rtl/gpu_gfx_top.sv`, `tests/kernels/vs_persp.s`, `tests/kernels/fs_texmod.s`,
`sim/tests/test_shader_pipeline.cpp`, `tools/sirion_run.cpp`, `docs/PROGRAMMING.md`.
**Files modified:** `rtl/graphics/raster.sv` (fragment buffer + emit mode + fragment-input ROP port),
`sim/run_rtl_tests.sh`, `Makefile` (runner targets, lint list).

---

## M20: the full programmable pipeline, sequenced in hardware ✅

**What's done:** the host mediation left over from M19 is gone. A **graphics command processor**
(`rtl/graphics/gfx_seq.sv`) executes an entire draw call autonomously:

```
draw ─► launch VS grid on the CUs ─► per triangle: fetch indices + transformed vertices
     ─► rasterize (fragment emit) ─► DMA fragments to global memory ─► write FS params
     ─► launch FS grid on the CUs ─► stream shaded fragments through the ROP ─► draw_done
```

- **Two programs resident at once:** the VS and FS binaries load at different entry points in one
  instruction memory (Sirion branches are PC-relative, so programs are position-independent); the launch
  descriptor picks the entry. The sequencer overwrites `c[0..2]` for the FS after the VS is done.
- **Host graphics API** (`sim/hostapi/sirion_device.hpp`): `loadProgram` (multiple resident),
  `dispatch(grid,tpb,entry)` for compute, `clear`, `draw(DrawDesc)` for graphics, `present(ppm)`, one
  object, **one command stream for compute AND graphics** interleaved freely.
- **The demo frame proves the unification:** a compute dispatch runs `gen_checker.k`, written in the
  **high-level language**, to procedurally generate the texture *on the GPU* (3.5K cycles), then two
  hardware draw calls render the perspective scene: opaque textured quad (45.5K cycles), translucent
  blended quad (15.8K cycles). Final framebuffer **pixel-exact** vs the stage-golden model;
  `build/render_hw_pipeline.ppm` is a frame produced end-to-end by the hardware.
- **Two toolchain bugs this milestone flushed out:** (1) `tri` is a SystemVerilog keyword (tri-state net), a `word_t tri;` produced a cascade of baffling syntax errors; (2) **the compiler silently truncated
  constants wider than MOVI's 19-bit immediate** (0x3050A0 became 0x50A0, caught by the texture
  known-answer check). `sirc.py` now materializes wide constants (MOVI/SHLI/ORI byte-builds) and `asm.py`
  **errors** on out-of-range immediates instead of truncating, a correctness fix for all future programs.

**Files added:** `rtl/graphics/gfx_seq.sv`, `sim/hostapi/sirion_device.hpp`,
`examples/kernels/gen_checker.k`, `sim/tests/test_gfx_pipeline.cpp`.
**Files modified:** `rtl/gpu_gfx_top.sv` (sequencer + host/sequencer muxes + draw command ports),
`rtl/graphics/raster.sv` (`frag_rst`), `scripts/sirc.py` (wide-constant materialization),
`scripts/asm.py` (immediate range checks), `sim/run_rtl_tests.sh`, `Makefile`.

---

## M21: compiler completeness ✅

**What's done:** `scripts/sirc.py` grew from an integer-only toy into a compiler you can write real GPU
programs in (see `docs/PROGRAMMING.md` for the full language):

- **`float` type + FP codegen**: float literals, mixed int/float expressions with automatic conversion
  (`I2F`/`F2I`), `FADD/FSUB/FMUL`, float `/` lowered to `MUFU.RCP` + `FMUL`, float comparisons via `FSETP`,
  casts `itof`/`ftoi`.
- **`__shared__ int buf[N]` + `barrier()`**: block-shared arrays lower to `LDS`/`STS`, the barrier to `BAR`.
- **Atomic intrinsics** `atomic_add/min/max/exch/cas(p, idx, …)` (return the old value; CAS's compare rides
  the `rd` convention) on global pointers or shared arrays.
- **SFU intrinsics** `rcpf/rsqrtf/sinf/cosf`.
- **Device functions, always inlined** (`func float sq(float v) { return v*v; }`), the strategy real GPU
  compilers default to; recursion is a compile error (depth-checked).
- **Register spilling**: locals beyond the register budget spill to **per-thread shared-memory slots**
  (`SMEM_TOP − (slot+1)·tpb·4 + tid·4`), with headroom reserved for expression temporaries, a 14-local
  kernel now compiles to 338 instructions with 28 spill accesses and runs bit-exact.
- **Constant folding** (integer), hex literals, and wide-constant materialization (from M20's bug fix).

**Hardware sizing follow-ups** the spill test forced: shared memory 4 KB → **16 KB** and instruction
memory 256 → **1024 words** (the 338-instruction spill kernel silently wrapped imem, the RTL returned
zeros while the ISS was fine; the cross-check caught it immediately).

**Deferred with rationale:** a `CALL`/`RET`-based ABI (non-inlined calls), inlining covers the language
semantics the way production GPU compilers do by default, and the ISA's CALL/RET remain available to
hand-written assembly.

**Tests:** the HL suite grew 3 → **8 kernels / 295 checks**, every new feature with a closed-form known
answer (float saxpy exact vs IEEE, 3-4-5/5-12-13 normalization via inlined `sq`+`rsqrtf`, per-block
shared-memory reduction with barrier, atomic histogram, and the spill kernel). The feature kernels were
also run on the **RTL GPU** via `sirion_run_rtl`, results bit-identical to the ISS.

**Files modified:** `scripts/sirc.py` (rewritten, ~700 lines), `sim/tests/test_hl.cpp` (+5 tests),
`sim/run_hl_tests.sh`, `rtl/compute/cu_core.sv` + `rtl/gpu_top.sv` + `rtl/gpu_gfx_top.sv` (SMEM/IMEM
sizing), `tools/sirion_run.cpp`, `docs/PROGRAMMING.md` (language section rewritten).
**Files added:** `examples/kernels/{saxpyf,normalize,reduce_shared,hist,spill}.k`.

---

## M22: FPGA synthesis-readiness capstone ✅

**What's done:** the project's stated arc is simulation → FPGA → tapeout; this milestone proves the RTL is
structurally ready for the second step with an all-open-source flow (`synth/synth.sh`, run from WSL):

1. **sv2v** converts the full SystemVerilog to Verilog-2005 with `-DSYNTHESIS`, everything under
   `` `ifndef SYNTHESIS `` (SVA, debug) drops out. Verified: the generated netlist contains **zero**
   `assert`/`$display` constructs.
2. **yosys elaborates the ENTIRE GPU** (`gpu_gfx_top`: CU array + L1s + L2 + dispatcher + rasterizer +
   graphics command processor): **77,517 cells / 84,406 wires**, 3,442 adders, 684 multipliers,
   78 dividers, 9,304 comparators, 45,086 muxes, and **26 `$mem` macros** (the BRAM candidates: register
   files, caches, imem/cbank/smem, framebuffers, texture pyramid, fragment buffer).
3. yosys runs generic **gate-level synthesis of the arithmetically-hardest execution units**
   to technology-independent netlists: `fp_alu` **12,138 cells**, `sfu` **34,820** (the exact-integer
   divider + 24-step isqrt dominate, as expected), integer `alu` **12,069**, `warp_sched` **87**.
   (Gate-mapping the whole CU is dominated by flattening the 131-Kbit register file into FF+mux trees, exactly the thing a device flow avoids by BRAM-mapping it, so the leaf-unit numbers are the
   meaningful ones.)

`docs/FPGA.md` records the deliberate simulation-model → device-flow substitutions: external DDR behind
the L2's already-request/ack seam, registered-read BRAMs (one added pipe stage per consumer), pipelined
dividers for the rasterizer/SFU, and an AXI-lite/UART bridge speaking the existing host port protocol.

**Toolchain notes:** yosys 0.33 via apt; sv2v v0.0.12 release binary (install commands in FPGA.md). The
full-GPU `proc` pass is dominated by the per-warp register files' 1024-bit write-enable vectors (~6 min);
running synthesis concurrently with the Verilator test suite overloads the WSL VM, run them sequentially.

**Files added:** `synth/synth.sh`, `docs/FPGA.md`.

---

## How it all fits together

Think of it as a contract, a reference, and parts:

1. **The contract** is the ISA, written once in C++ (`sim/iss/isa.hpp`) and once in SV
   (`rtl/sirion_pkg.sv`), and documented in `docs/ISA.md`.
2. **The reference** is the ISS (`sim/iss/iss.hpp`): give it a `.gpubin` + inputs and it produces the
   correct final memory/register state. The **assembler** (`scripts/asm.py`) produces those binaries from
   readable kernels.
3. **The parts** are the M2 RTL modules, the ALU, register file, decoder, scoreboard, FIFO, each proven
   correct in isolation against the *same* golden functions the reference uses.

Those M2 parts are now **wired together** in `cu_core.sv`, a multi-warp SIMT compute unit (M3–M6, M11), and separately extended by the cache/scheduler/FP/rasterizer milestones (M7–M10). Every module test is
self-checking (directed cases pin corners; seeded-random sweeps the space; in-RTL assertions catch invariant
violations), and the integrated core is diffed bit-exact against the golden ISS for every kernel.

Run it all: `bash run_all.sh` → `SIRION: ALL TESTS PASSED` (ISS 5/5 + high-level 3/3 + all RTL groups),
`make lint` clean, `make png` to view a render, `make wavetext` to see a waveform as text.

---

## Status & future work

**A complete, verified GPU.** Everything below exists and is tested in **both** MSYS2 (Verilator 5.040)
and WSL (5.020); `bash run_all.sh` → `SIRION: ALL TESTS PASSED`.

- **Compute machine:** a **multi-CU array** of **multi-warp** (8 resident warps) SIMT cores with a
  **barrel pipeline** (~1 IPC/warp aggregate), hardware reconvergence + predication, real cross-warp
  barriers, a **coalescing write-through L1** per CU, a shared **write-back L2** that is the coherence and
  **atomics** point, **floating point + FFMA** and an **SFU** (RCP/RSQRT/SIN/COS) in every lane, and
  **hardware block/grid dispatch**, measured 2.93× scaling on 4 CUs.
- **Graphics:** a rasterizer with **perspective-correct interpolation, bilinear + mipmap filtering**, and a
  ROP (depth/blend), and, above it, **programmable shading**: vertex + fragment shaders run as *kernels on
  the compute units*, sequenced end-to-end by an on-chip **graphics command processor** (`gfx_seq`). One
  command stream drives compute and draw; frames are pixel-exact vs the golden pipeline.
- **Toolchain:** assembler + a **C-like compiler** with `float`, `__shared__`/`barrier()`, atomic + SFU
  intrinsics, inlined device functions, register spilling, constant folding; **CLI runners** execute any
  `.gpubin` on the ISS (`sirion_run`) or the RTL GPU (`sirion_run_rtl`) with perf counters
  (`docs/PROGRAMMING.md` is the programmer's guide).
- **Verification:** one golden ISS + shared reference functions; every milestone bit-exact (pixel-exact for
  graphics) with unbiased known-answer tests; SVA in all RTL; lint-clean; both platforms.
- **FPGA:** sv2v + yosys synthesis-readiness flow (`synth/synth.sh`, `docs/FPGA.md`).

**Deferred / stretch (not built):** MMU/TLB virtual addressing,
geometry/tessellation stages, non-inlined CALL/RET ABI, GTO scheduling, trilinear filtering + texture
compression.
