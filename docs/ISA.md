# Sirion ISA v1: Instruction Set Reference

This is the authoritative encoding reference for the Sirion GPU. It is kept in lockstep with
three implementations, and the assemble→run self-checking tests fail if any drifts:

- `rtl/sirion_pkg.sv`: RTL opcode enum + field extractors (hardware decode).
- `sim/iss/isa.hpp`: C++ decode/encode used by the golden ISS.
- `scripts/asm.py`: the Python assembler.

---

## 0. Background (read first)

Sirion executes **SIMT** (Single Instruction, Multiple Thread) programs. Threads are grouped into
**warps** of `WARP_SIZE = 32`. All lanes of a warp share one program counter and execute the same
instruction each step; an **active mask** says which lanes are on. Per-lane branches are handled by
**predication** (a per-lane on/off guard) and, for real control-flow divergence, by a hardware
**reconvergence stack** (§8). This is the same idea as NVIDIA warps and AMD wavefronts.

Threads are organized as a **grid of workgroups** (a.k.a. thread blocks / CTAs). A workgroup's warps
are co-resident and share **shared memory** and **barriers**. See `WALKTHROUGH.md` and `datapath.md`
for the microarchitecture; this document is only the ISA.

---

## 1. Register & state model

| State | Count | Width | Notes |
|-------|-------|-------|-------|
| Vector GPRs `R0..R15` | 16 / thread | 32b | per-lane general registers |
| Scalar/uniform regs `SR0..SR15` | 16 / warp | 32b | reserved for M3+ (uniform datapath) |
| Predicate regs `P0..P3` | 4 / warp | 1b/lane | **P0 == PT**, hardwired true, read-only |
| PC | 1 / warp | word index | v1 PC is an instruction (word) index; byte addr = PC·4 |

The architecture reserves 5-bit register fields (up to 32 GPRs); v1 uses 16 (4-bit fields).

---

## 2. Instruction word (fixed 32 bits)

Every instruction shares a 9-bit prefix: a 6-bit **opcode** and a 3-bit **guard** (predication).

```
 31        26 25   24 23 22                                   0
+------------+---+------+--------------------------------------+
|   opcode   |neg| psel |            format-specific           |
+------------+---+------+--------------------------------------+
     6         1    2                   23
```

- `neg` (bit 25) and `psel` (bits 24:23) form the **guard**: the instruction executes on lane *l* iff
  `(psel==0 ? 1 : P[psel][l]) XOR neg`. `psel==0` selects PT (always true), so no-prefix = unconditional.

### Format field layouts (bits 22:0)

| Format | Used by | 22:19 | 18:15 | 14:11 | 10:0 |
|--------|---------|-------|-------|-------|------|
| **R**  | ADD…SEQ | `rd` | `rs1` | `rs2` | none |
| **R1** | MOV, NOT | `rd` | `rs1` | none | none |
| **I**  | ADDI…SLTIU | `rd` | `rs1` | `imm15` (bits 14:0, signed) | |
| **U**  | MOVI, RDSR | `rd` | `imm19` (bits 18:0, signed / srid) | | |
| **B**  | BRA, SSY, CALL | `off23` (bits 22:0, signed, in instruction words) | | | |
| **L**  | LDG, LDS | `rd` | `rs1`(base) | `imm15`(offset, signed) | |
| **LDC**| LDC | `rd` | `imm19` (const-bank **word** index) | | |
| **S**  | STG, STS | `rd`(=data) | `rs1`(base) | `imm15`(offset, signed) | |
| **P**  | ISETP | `pd`(22:21) | `cmp`(20:18) | `rs1`(17:14) | `rs2`(13:10) |
| **Pi** | ISETPI | `pd`(22:21) | `cmp`(20:18) | `rs1`(17:14) | `imm14`(13:0, signed) |

Immediates are sign-extended to 32 bits. Branch/SSY targets are **PC-relative in instruction words**:
`target = PC_of_this_insn + off23`.

---

## 3. Opcode map

| Hex | Mnemonic | Fmt | Operation (per active lane) |
|-----|----------|-----|-----------------------------|
| 00 | `NOP`  | N  | nothing |
| 01 | `EXIT` | N  | mark executing lanes terminated |
| 02 | `BAR`  | N  | workgroup barrier (M6; no-op for single-warp kernels) |
| 03 | `BRA`  | B  | branch guard-true lanes to target (divergence point, §8) |
| 04 | `SSY`  | B  | push reconvergence point = target for the next divergent branch (§8) |
| 05 | `SYNC` | N  | force reconverge to current frame's rpc (rarely needed) |
| 06 | `CALL` | B  | call (M3+); v1 ISS treats as branch |
| 07 | `RET`  | N  | return (M3+) |
| 08 | `RDSR` | U  | `Rd = special_reg[srid]` (§5) |
| 10 | `ADD`  | R  | `Rd = Rs1 + Rs2` |
| 11 | `SUB`  | R  | `Rd = Rs1 - Rs2` |
| 12 | `MUL`  | R  | `Rd = (Rs1 * Rs2)[31:0]` |
| 13 | `MULH` | R  | `Rd = (sext(Rs1) * sext(Rs2))[63:32]` (signed high) |
| 14 | `AND`  | R  | `Rd = Rs1 & Rs2` |
| 15 | `OR`   | R  | `Rd = Rs1 \| Rs2` |
| 16 | `XOR`  | R  | `Rd = Rs1 ^ Rs2` |
| 17 | `NOT`  | R1 | `Rd = ~Rs1` |
| 18 | `SHL`  | R  | `Rd = Rs1 << (Rs2 & 31)` |
| 19 | `SHR`  | R  | `Rd = Rs1 >> (Rs2 & 31)` (logical) |
| 1A | `SRA`  | R  | `Rd = Rs1 >>> (Rs2 & 31)` (arithmetic) |
| 1B | `SLT`  | R  | `Rd = (int)Rs1 < (int)Rs2 ? 1 : 0` |
| 1C | `SLTU` | R  | `Rd = (uint)Rs1 < (uint)Rs2 ? 1 : 0` |
| 1D | `MIN`  | R  | signed min |
| 1E | `MAX`  | R  | signed max |
| 1F | `SEQ`  | R  | `Rd = (Rs1 == Rs2) ? 1 : 0` |
| 20 | `ADDI` | I  | `Rd = Rs1 + imm` |
| 21 | `ANDI` | I  | `Rd = Rs1 & imm` |
| 22 | `ORI`  | I  | `Rd = Rs1 \| imm` |
| 23 | `XORI` | I  | `Rd = Rs1 ^ imm` |
| 24 | `SHLI` | I  | `Rd = Rs1 << (imm & 31)` |
| 25 | `SHRI` | I  | `Rd = Rs1 >> (imm & 31)` (logical) |
| 26 | `SRAI` | I  | `Rd = Rs1 >>> (imm & 31)` (arithmetic) |
| 27 | `SLTI` | I  | signed set-less-than immediate |
| 28 | `SLTIU`| I  | unsigned set-less-than immediate |
| 29 | `MOVI` | U  | `Rd = sext(imm19)` |
| 2A | `MOV`  | R1 | `Rd = Rs1` |
| 2B | `ISETP`| P  | `P[pd][lane] = compare(cmp, Rs1, Rs2)` (§4) |
| 2C | `ISETPI`| Pi | `P[pd][lane] = compare(cmp, Rs1, imm14)` |
| 2D | `MUFU.fn` | R1+fn | SFU: `Rd = fn(Rs1)`; fn in bits [2:0]: 0=RCP 1=RSQRT 2=SIN 3=COS (M16, §5.1) |
| 30 | `LDG`  | L  | `Rd = mem32[Rs1 + off]` (global) |
| 31 | `STG`  | S  | `mem32[Rs1 + off] = Rd` (global; Rd field = data) |
| 32 | `LDS`  | L  | `Rd = smem32[Rs1 + off]` (shared; M6) |
| 33 | `STS`  | S  | `smem32[Rs1 + off] = Rd` (shared; M6) |
| 34 | `LDC`  | LDC| `Rd = const_bank[imm19]` (word-indexed; broadcast) |
| 35 | `FADD` | R  | `Rd = Rs1 +f Rs2` (binary32; M15, §5) |
| 36 | `FSUB` | R  | `Rd = Rs1 -f Rs2` |
| 37 | `FMUL` | R  | `Rd = Rs1 *f Rs2` |
| 38 | `FFMA` | R  | `Rd = Rs1 *f Rs2 +f Rd`; **Rd is the accumulator input** (3rd RF read port) |
| 39 | `FMIN` | R  | float min (total order on FTZ-canonicalized keys) |
| 3A | `FMAX` | R  | float max |
| 3B | `I2F`  | R1 | `Rd = float(int Rs1)` (truncating) |
| 3C | `F2I`  | R1 | `Rd = int(float Rs1)` (truncate toward zero, saturating) |
| 3D | `FSETP`| P  | `P[pd][lane] = fcompare(cmp, Rs1, Rs2)`, float ISETP; LTU/GEU reserved |
| 3E | `ATOMG.fn` | R+fn | global atomic RMW: `old = mem32[Rs1]; mem32[Rs1] = fn(old, Rs2[, Rd]); Rd = old`; fn in bits [10:8]: 0=ADD 1=MIN 2=MAX 3=EXCH 4=CAS (compare value passed in Rd); lanes commit in lane order (M16) |
| 3F | `ATOMS.fn` | R+fn | same on shared memory |

### 3.1 Floating-point semantics (M15)

Binary32 with **round-toward-zero** and **flush-to-zero** denormals; Inf/NaN are out of
scope (the `rcp(0)`/`rsqrt(0)` Inf bit patterns are produced but never consumed specially).
Fully deterministic: the ISS reference functions (`fp_*_ref` in `sim/iss/isa.hpp`) and the
RTL (`fp_alu.sv`) implement the identical integer algorithms, bit for bit.

### 3.2 SFU accuracy (M16)

`RCP` is an exact truncated reciprocal (≤1 ULP); `RSQRT` a truncated integer square root
followed by RCP (≤~2 ULP); `SIN`/`COS` use a quarter-wave 65-entry Q15 table with linear
interpolation over a Q16 fraction-of-turn phase (~1e-4 absolute; mirrors `sfu_*_ref`).

Opcodes `09–0F`, `2D–2F`, `35–3F` are reserved for FP/SFU/atomic/texture (M8+). Writing `P0` (PT)
with ISETP is ignored (PT is read-only true).

---

## 4. Compare codes (ISETP `cmp` field)

| Code | Suffix | Meaning |
|------|--------|---------|
| 0 | `.EQ`  | `a == b` |
| 1 | `.NE`  | `a != b` |
| 2 | `.LT`  | signed `a < b` |
| 3 | `.LE`  | signed `a <= b` |
| 4 | `.GT`  | signed `a > b` |
| 5 | `.GE`  | signed `a >= b` |
| 6 | `.LTU` | unsigned `a < b` |
| 7 | `.GEU` | unsigned `a >= b` |

---

## 5. Special registers (`RDSR Rd, name`)

| idx | name | value |
|-----|------|-------|
| 0–2 | `TID_X/Y/Z` | thread index within workgroup |
| 3–5 | `NTID_X/Y/Z` | workgroup dimensions (`blockDim`) |
| 6–8 | `CTAID_X/Y/Z` | workgroup index within grid |
| 9–11 | `NCTAID_X/Y/Z` | grid dimensions (`gridDim`) |
| 12 | `LANEID` | lane index within the warp (0–31) |
| 13 | `WARPID` | warp index within the workgroup |
| 14 | `TID_FLAT` | global linear thread id = `block_linear·threadsPerBlock + tid_linear` |

---

## 6. Memory model

- **Global** memory is flat, byte-addressed, little-endian. `LDG/STG` transfer aligned 32-bit words.
  Per lane the address is `Rs1 + sext(off)`. (Coalescing across lanes is a microarchitectural concern,
  added in M5; the ISA semantics are per-lane independent accesses.)
- **Shared** memory (`LDS/STS`) is per-workgroup, word-indexed by `(Rs1+off) >> 2` (M6).
- **Constant bank** (`LDC`) is a word-indexed, read-only, uniformly-broadcast array. Kernel parameters
  (pointers/scalars) are placed here by the host at launch, starting at word 0; static `.const` data
  from the object file follows. This is how kernels obtain their argument pointers.

---

## 7. Predication

Any instruction may be guarded: `@P1 ADD R1,R2,R3` executes only on lanes where `P1` is set; `@!P2 …`
negates. Guarded lanes that are off produce no register/memory side effects. Predicates are produced by
`ISETP`/`ISETPI`. Unconditional instructions implicitly use `PT` (`psel==0`).

---

## 8. Control flow & reconvergence

`BRA` branches the **guard-true** lanes to the target; guard-false lanes fall through. If a warp's active
lanes all agree, the branch is uniform (no divergence). If they split, the hardware uses an **IPDOM
reconvergence stack**:

1. The assembler emits `SSY R` before a potentially-divergent branch, naming the reconvergence point `R`
   (the immediate post-dominator, where the paths merge).
2. On a divergent `BRA`, the current stack frame becomes the **join frame** (holds the full pre-divergence
   mask, resumes at `R`, keeps its outer reconvergence target); two child frames are pushed for the taken
   and fall-through paths, each with `rpc = R`.
3. A child frame pops when its PC reaches `R`; when both have popped, the join frame resumes at `R` with the
   reconverged mask. Nesting and loops work because each frame carries its own `rpc`.

`EXIT` permanently deactivates the executing lanes. See `sim/iss/iss.hpp` for the reference algorithm and
`tests/kernels/abs.s` (nested divergence) / `loop_reduce.s` (loop) for worked examples.

---

## 9. Object format (`.gpubin`)

Little-endian. 32-byte header then code and constant words (see `sim/iss/gpubin.hpp`, `scripts/asm.py`):

```
char   magic[4] = "SGPU"
u16    version=1 ; u16 flags=0
u32    num_insns ; u32 num_const ; u32 entry ; u32 reg_count ; u32 shared_bytes ; u32 reserved
u32    code[num_insns]
u32    const[num_const]
```

v1 is position-independent (word-relative branches) so no relocations are needed; `version`/`flags`
reserve room for them.

---

## 10. Assembly syntax

- Comments: `;` or `//` to end of line. Labels: `name:` (own line or before an instruction).
- Guard prefix: `@P1` / `@!P2` before the mnemonic.
- Registers `R0..R15`, predicates `P0..P3`. Immediates: decimal, `0x` hex, optional leading `#`, may be negative.
- Memory: `[R3]` or `[R3+16]` / `[R3+0x10]`. Constant: `c[3]`. ISETP: `ISETP.GE P1, R0, R1` (or immediate → `ISETPI`).

Worked example (`tests/kernels/vec_add.s`):

```asm
        RDSR     R0, TID_FLAT        ; i = global thread id
        LDC      R1, c[3]            ; n
        ISETP.GE P1, R0, R1          ; P1 = (i >= n)
        SSY      Ldone
@P1     BRA      Ldone               ; out-of-range lanes skip
        SHLI     R2, R0, 2           ; i*4
        LDC      R3, c[1]            ; a_ptr
        ...
        STG      [R5], R8
Ldone:  EXIT
```
