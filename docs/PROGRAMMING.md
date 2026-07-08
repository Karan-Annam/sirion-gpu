# Programming the Sirion GPU

This is the hands-on guide: how to **write a program, compile it, and run it** on the Sirion
GPU, first on the golden simulator (instant), then on the real RTL under Verilator. No C++
required for any of it.

All commands run from `gpu_project/` in an MSYS2/Git-Bash or WSL shell.

```
 your_kernel.k ──sirc.py──► your_kernel.s ──asm.py──► your_kernel.gpubin ──► sirion_run      (ISS)
 (C-like source)            (Sirion assembly)          (binary)              sirion_run_rtl  (RTL GPU)
```

---

## 0. One-time setup

```bash
make runner        # builds build/sirion_run.exe      (golden-ISS runner, instant)
make runner-rtl    # builds build/sirion_run_rtl.exe  (the REAL RTL GPU under Verilator)
```

## 1. Your first kernel, start to finish

Create `my_add.k`:

```c
kernel my_add(int* out, int* a, int* b, int n) {
    int i = tid();
    if (i < n) {
        out[i] = a[i] + b[i];
    }
}
```

Compile it (high-level → assembly → binary):

```bash
python scripts/sirc.py my_add.k -o build/my_add.s      # look inside build/my_add.s!
python scripts/asm.py build/my_add.s -o build/my_add.gpubin
```

Run it. Parameters are passed in the **constant bank** in declaration order
(`out`→`c[0]`, `a`→`c[1]`, `b`→`c[2]`, `n`→`c[3]`). You choose where buffers live in the
GPU's global memory (byte addresses; keep them apart):

```bash
./build/sirion_run.exe build/my_add.gpubin --grid 4 --tpb 32 \
    --const 0=0x3000 --const 1=0x1000 --const 2=0x2000 --const 3=100 \
    --fill 0x1000:100:1:1  --fill 0x2000:100:2:3 \
    --dump 0x3000:8
# [mem 0x3000] 3 7 11 15 19 23 27 31        <- out[i] = (i+1) + (3i+2)
```

Same thing on the **real RTL GPU** (multi-warp barrel pipeline, L1/L2 caches, the works),
with live performance counters:

```bash
./build/sirion_run_rtl.exe build/my_add.gpubin --grid 4 --tpb 32 \
    --const 0=0x3000 --const 1=0x1000 --const 2=0x2000 --const 3=100 \
    --fill 0x1000:100:1:1 --fill 0x2000:100:2:3 --dump 0x3000:8
# [sirion_run] grid done: 891 cycles, 68 insns, 12 memops, L1 0/26 hit/miss, L2 0/39 ...
```

Runner options:

| option | meaning |
|--------|---------|
| `--grid N` / `--tpb N` | blocks in the grid / threads per block (≤ 256 = 8 warps) |
| `--const IDX=VAL` | set kernel parameter `c[IDX]` |
| `--store ADDR=VAL` | write one word of global memory before launch |
| `--fill ADDR:COUNT:BASE:STEP` | write COUNT words: BASE, BASE+STEP, … |
| `--dump ADDR:COUNT` / `--dumpf ...` | print memory after the run (as ints / as floats) |

`VAL` accepts decimal, `0x` hex, or a float literal like `1.5f` (stored as its bit pattern).

---

## 2. The execution model (what your program runs on)

Sirion is a SIMT GPU, programmed like CUDA/OpenCL:

- A **launch** runs a 1-D **grid** of **blocks**; each block has `tpb` **threads**
  (up to 256). The hardware dispatcher hands blocks to free **compute units**.
- Threads execute in **warps of 32** in lock-step. `if`/`while` on thread-dependent values
  work fine, the hardware masks lanes and reconverges (it costs performance, not
  correctness).
- Each thread has its own 16 registers. Blocks share **shared memory** (4 KB) and
  synchronize with a **barrier**. All blocks share **global memory** through the caches.
- Memory visibility: within a block, use the barrier. Across blocks, results are visible
  when the grid finishes (or use atomics).

Builtins available in the C-like language:

| builtin | value |
|---------|-------|
| `tid()` | global flat thread id (`ctaid()*ntid() + thread-in-block`) |
| `lane()` | lane id within the warp (0–31) |
| `ctaid()` | block index |
| `ntid()` | threads per block |
| `nctaid()` | blocks in the grid |

## 3. The C-like language (`.k` files, `scripts/sirc.py`)

- **Types**: `int`, `float` (binary32), and pointer parameters `int*` / `float*`. Mixed
  int/float expressions convert the int side automatically; explicit casts: `itof(e)`,
  `ftoi(e)` (truncates).
- **Operators**: `+ - *` (int and float), `/` (float only, compiled to reciprocal ×
  multiply; there is no integer divide in the ISA, use shifts), `& | ^ ~ << >>` (int),
  unary `-`, comparisons `< <= > >= == !=` (int `ISETP` / float `FSETP`).
- **Control**: `if`/`else`, `while`, `for`.
- **Memory**: array indexing on pointer params (`a[i]`, `out[i] = ...`);
  `__shared__ int buf[256];` block-shared arrays; `barrier();` block barrier.
- **Atomics** (return the old value): `atomic_add(p, idx, val)`, `atomic_min`,
  `atomic_max`, `atomic_exch(p, idx, val)`, `atomic_cas(p, idx, cmp, val)`. `p` is a
  pointer param or `__shared__` array.
- **SFU intrinsics** (float): `rcpf(x)`, `rsqrtf(x)`, `sinf(x)`, `cosf(x)`.
- **Device functions**, always inlined (as real GPU compilers default):

```c
func float sq(float v) { return v * v; }

kernel normalize(float* out, float* x, float* y, int n) {
    int i = tid();
    if (i < n) {
        float r = rsqrtf(sq(x[i]) + sq(y[i]));
        out[i] = x[i] * r;
    }
}
```

- **Register spilling**: locals beyond the register budget spill automatically to
  per-thread shared-memory slots, many-local kernels compile and run correctly (just
  slower). Very deep single *expressions* can still exhaust temporaries; split them.
- **Constants** of any 32-bit width work (wide ones are materialized in several
  instructions).

More examples in `examples/kernels/`: `reduce_shared.k` (shared memory + barrier),
`hist.k` (atomics), `saxpyf.k` (floats), `gen_checker.k` (procedural texture, used by
the M20 graphics demo), `spill.k` (14 chained locals). Look at the generated `.s`, it is
the best way to learn the assembly.

## 4. Sirion assembly (`.s` files, `scripts/asm.py`)

Everything the hardware can do is reachable from assembly. Registers `R0..R15` (per
thread), predicates `P1..P3` (`P0` is constant true). Any instruction can be guarded:
`@P1 BRA target` (execute where P1 set), `@!P1 EXIT` (where clear).

### Instruction quick reference

| group | instructions |
|-------|--------------|
| integer | `ADD SUB MUL MULH AND OR XOR NOT SHL SHR SRA SLT SLTU MIN MAX SEQ` (3-reg) |
| immediates | `ADDI ANDI ORI XORI SHLI SHRI SRAI SLTI SLTIU` (`op Rd, Rs, #imm15`), `MOVI Rd, #imm19`, `MOV Rd, Rs` |
| predicates | `ISETP.cc Pd, Ra, Rb_or_imm` / `FSETP.cc Pd, Ra, Rb`, cc ∈ `EQ NE LT LE GT GE LTU GEU` |
| control | `BRA label` `SSY label` `EXIT` `BAR` (`SSY` marks the reconvergence point **before** a divergent branch) |
| special regs | `RDSR Rd, TID_FLAT|LANEID|WARPID|TID_X|NTID_X|CTAID_X|NCTAID_X` |
| memory | `LDG/STG Rd, [Rb+off]` global (byte addr) · `LDS/STS` shared · `LDC Rd, c[i]` params |
| float (binary32) | `FADD FSUB FMUL FMIN FMAX` (3-reg) · `FFMA Rd, Ra, Rb` (Rd = Ra*Rb + **Rd**) · `I2F F2I Rd, Rs` |
| SFU | `MUFU.RCP/RSQRT/SIN/COS Rd, Rs` (approximate transcendentals) |
| atomics | `ATOMG.fn Rd, [Rb], Rs` global / `ATOMS.fn` shared, fn ∈ `ADD MIN MAX EXCH CAS`; `Rd` gets the OLD value; for CAS the compare value is passed in `Rd` |

FP semantics: round-toward-zero, denormals flush to zero, Inf/NaN out of scope. Fully
deterministic and identical between the ISS and the RTL.

### The divergence pattern

Every thread-dependent branch needs an `SSY` naming where the paths re-join:

```asm
        ISETP.GE P1, R0, R1     ; P1 = (i >= n)
        SSY      Ldone          ; declare the reconvergence point
@P1     BRA      Ldone          ; guard-true lanes leave; others fall through
        ...                     ; divergent work
Ldone:
        EXIT
```

### A complete assembly kernel (histogram with atomics)

`tests/kernels/histogram.s`, every thread atomically increments a bin; works across all
warps, blocks, and compute units because atomics serialize at the shared L2:

```asm
        RDSR     R0, TID_FLAT
        LDC      R1, c[2]            ; n
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone
        SHLI     R2, R0, 2
        LDC      R3, c[1]            ; data base
        ADD      R3, R3, R2
        LDG      R4, [R3]            ; v = data[i]
        SHLI     R4, R4, 2
        LDC      R5, c[0]            ; hist base
        ADD      R5, R5, R4
        MOVI     R6, 1
        ATOMG.ADD R7, [R5], R6       ; hist[v] += 1 atomically
Ldone:  EXIT
```

```bash
python scripts/asm.py tests/kernels/histogram.s -o build/histogram.gpubin
# 64 data values cycling 0..15 four times (four fills of 16) -> every bin must count 4
./build/sirion_run.exe build/histogram.gpubin --grid 2 --tpb 32 \
    --const 0=0x200 --const 1=0x1000 --const 2=64 \
    --fill 0x1000:16:0:1 --fill 0x1040:16:0:1 --fill 0x1080:16:0:1 --fill 0x10C0:16:0:1 \
    --dump 0x200:16
# [mem 0x200] 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4 4
```

More studied examples in `tests/kernels/`: `saxpy_float.s` (FFMA), `rsqrt_map.s` (SFU),
`block_shuffle.s` (shared memory + barrier across warps), `m6_shared.s` (shared memory),
`vs_persp.s` + `fs_texmod.s` (the M19 vertex/fragment **shaders**, shaders are just
kernels here).

## 5. Memory map & parameter conventions

- **Global memory**: byte-addressed; the runner's device has 32 KB (`gpu_top` default), keep buffers under `0x8000`. Buffers are wherever you put them; pass base addresses as
  parameters through the constant bank.
- **Constant bank** (`c[0..63]`): kernel parameters, set by the host (`--const`). `LDC` is
  word-indexed.
- **Shared memory**: 4 KB per block, byte-addressed via `LDS`/`STS`, zero-initialized per
  launch, synchronized with `BAR`.
- Loads/stores are 32-bit words; addresses should be 4-byte aligned.

## 6. Performance tips (they are measurable, the RTL runner prints counters)

- **Coalesce**: consecutive lanes reading consecutive words → one L1 line transaction per
  8 lanes. Strided or random access serializes into more transactions.
- **Occupancy**: 256 threads/block = 8 resident warps = the barrel pipeline can issue every
  cycle and hide memory latency. Tiny blocks leave the pipeline idle.
- **Divergence**: a fully divergent warp runs each path serially; the memory unit walks
  only active lanes, but ALU issue is per-warp.
- **Reuse**: the L1 keeps 64 lines of 8 words; sequential per-lane walks (see
  `loop_reduce.s`) hit ~87% in practice.
- Compare for yourself: run the same kernel with `--tpb 32` vs `--tpb 256` on
  `sirion_run_rtl` and watch cycles/IPC.

## 7. Verifying like the test suite does

For anything beyond quick experiments, write both a kernel and a known answer, then check
the ISS and RTL agree (`sim/tests/test_cu_core.cpp` is the template). The project's rule:
**RTL == ISS bit-exact, and ISS == closed-form known answer.** `bash run_all.sh` runs
everything.

## 8. Debugging

- **Look at the assembly**: `python scripts/sirc.py my.k -o /dev/stdout`
- **ISS first**: logic bugs reproduce on `sirion_run.exe` in milliseconds.
- **Waveforms**: testbench-driven runs write VCDs; view with `make wavetext` (no GUI) or
  GTKWave (see README).
- **Common gotchas**:
  - forgot `SSY` before a divergent `BRA` → lanes never reconverge, kernel may spin;
  - `LDC` is **word**-indexed (`c[3]`), `LDG` is **byte**-addressed;
  - `FFMA Rd, Ra, Rb` uses **Rd** as the accumulator input (read-modify-write);
  - guards: `@P1` runs where the bit is **set**; `@!P1` where clear;
  - immediates are 15-bit signed, `MOVI` (19-bit) or build wider values with `SHLI`+`OR`.
