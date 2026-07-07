// isa.hpp — Sirion ISA v1 encoding (C++ side), shared by the ISS and the iss tool.
//
// This MUST stay in lockstep with rtl/sirion_pkg.sv (RTL decode) and scripts/asm.py
// (assembler). The self-checking tests (assemble -> run) catch any drift.
//
// Fixed 32-bit instructions. Layout (MSB..LSB):
//   [31:26] opcode (6)              [25] guard.neg   [24:23] guard.psel (P0..P3, P0==PT)
// Format-specific fields below the guard:
//   R-type : [22:19] rd  [18:15] rs1 [14:11] rs2
//   I-type : [22:19] rd  [18:15] rs1 [14:0]  imm15 (signed)
//   U-type : [22:19] rd  [18:0]  imm19 (signed)          (MOVI, RDSR:srid)
//   B-type : [22:0]  off23 (signed, in instruction words) (BRA/SSY/CALL)
//   M-load : [22:19] rd  [18:15] rs1(base) [14:0] imm15   (LDG/LDS)
//   M-store: [22:19] rs2(data) [18:15] rs1(base) [14:0] imm15 (STG/STS)
//   LDC    : [22:19] rd  [18:0] imm19 (const-bank WORD index)
//   P-type : [22:21] pd  [20:18] cmp  [17:14] rs1 [13:10] rs2 (ISETP)
//   Pi-type: [22:21] pd  [20:18] cmp  [17:14] rs1 [13:0] imm14 (ISETPI)
#pragma once
#include <cstdint>

namespace sirion {

enum Opcode : uint8_t {
  // control / misc (0x00-0x0F)
  OP_NOP  = 0x00, OP_EXIT = 0x01, OP_BAR = 0x02, OP_BRA = 0x03,
  OP_SSY  = 0x04, OP_SYNC = 0x05, OP_CALL = 0x06, OP_RET = 0x07, OP_RDSR = 0x08,
  // integer ALU, register-register (0x10-0x1F)
  OP_ADD  = 0x10, OP_SUB = 0x11, OP_MUL = 0x12, OP_MULH = 0x13,
  OP_AND  = 0x14, OP_OR  = 0x15, OP_XOR = 0x16, OP_NOT  = 0x17,
  OP_SHL  = 0x18, OP_SHR = 0x19, OP_SRA = 0x1A, OP_SLT  = 0x1B,
  OP_SLTU = 0x1C, OP_MIN = 0x1D, OP_MAX = 0x1E, OP_SEQ  = 0x1F,
  // integer ALU, immediate + moves + predicate-set (0x20-0x2C)
  OP_ADDI = 0x20, OP_ANDI = 0x21, OP_ORI = 0x22, OP_XORI = 0x23,
  OP_SHLI = 0x24, OP_SHRI = 0x25, OP_SRAI = 0x26, OP_SLTI = 0x27,
  OP_SLTIU= 0x28, OP_MOVI = 0x29, OP_MOV = 0x2A, OP_ISETP = 0x2B, OP_ISETPI = 0x2C,
  // memory (0x30-0x34)
  OP_LDG  = 0x30, OP_STG = 0x31, OP_LDS = 0x32, OP_STS = 0x33, OP_LDC = 0x34,
  // floating point, binary32 (0x35-0x3D) — M15. Round-toward-zero, flush-to-zero
  // denormals, Inf/NaN out of scope (see fp_*_ref below; RTL is bit-identical).
  OP_FADD = 0x35, OP_FSUB = 0x36, OP_FMUL = 0x37, OP_FFMA = 0x38,  // FFMA: rd = rs1*rs2 + rd
  OP_FMIN = 0x39, OP_FMAX = 0x3A, OP_I2F  = 0x3B, OP_F2I  = 0x3C,
  OP_FSETP= 0x3D,                                                   // float ISETP (P-format)
  // M16: special-function unit + atomics.
  OP_MUFU = 0x2D,   // MUFU.func rd, rs1 — func in insn[2:0] (RCP/RSQRT/SIN/COS)
  OP_ATOMG= 0x3E,   // ATOMG.func rd, [rs1], rs2 — global atomic RMW, rd <- old value
  OP_ATOMS= 0x3F,   //   ATOMS: same on shared memory. func in insn[10:8]; CAS compare = rd.
};

// MUFU function codes (insn[2:0]).
enum Mufu : uint8_t { MUFU_RCP = 0, MUFU_RSQRT = 1, MUFU_SIN = 2, MUFU_COS = 3 };
// Atomic function codes (insn[10:8]). Lanes commit in lane order (0..31).
enum AFunc : uint8_t { ATOM_ADD = 0, ATOM_MIN = 1, ATOM_MAX = 2, ATOM_EXCH = 3, ATOM_CAS = 4 };

inline uint32_t atom_ref(uint8_t fn, uint32_t old, uint32_t val, uint32_t cmp) {
  switch (fn) {
    case ATOM_ADD:  return old + val;
    case ATOM_MIN:  return ((int32_t)val < (int32_t)old) ? val : old;
    case ATOM_MAX:  return ((int32_t)val > (int32_t)old) ? val : old;
    case ATOM_EXCH: return val;
    case ATOM_CAS:  return (old == cmp) ? val : old;
    default:        return old;
  }
}

// ISETP comparison codes (3 bits).
enum Cmp : uint8_t {
  CMP_EQ = 0, CMP_NE = 1, CMP_LT = 2, CMP_LE = 3,
  CMP_GT = 4, CMP_GE = 5, CMP_LTU = 6, CMP_GEU = 7,
};

// Special registers (read with RDSR rd, #srid).
enum Sreg : uint8_t {
  SR_TID_X = 0,  SR_TID_Y = 1,  SR_TID_Z = 2,
  SR_NTID_X = 3, SR_NTID_Y = 4, SR_NTID_Z = 5,
  SR_CTAID_X = 6, SR_CTAID_Y = 7, SR_CTAID_Z = 8,
  SR_NCTAID_X = 9, SR_NCTAID_Y = 10, SR_NCTAID_Z = 11,
  SR_LANEID = 12, SR_WARPID = 13, SR_TID_FLAT = 14,
};

static constexpr int WARP_SIZE = 32;
static constexpr int NUM_VREGS = 16;
static constexpr int NUM_PREDS = 4;   // P0..P3, P0 == PT (hardwired true)
static constexpr int PRED_TRUE = 0;

// Thin decoder over a raw 32-bit instruction word.
struct Insn {
  uint32_t raw;
  Insn() : raw(0) {}
  explicit Insn(uint32_t r) : raw(r) {}

  uint8_t opcode() const { return (raw >> 26) & 0x3F; }
  bool    gneg()   const { return (raw >> 25) & 0x1; }
  uint8_t psel()   const { return (raw >> 23) & 0x3; }

  uint8_t rd()  const { return (raw >> 19) & 0xF; }
  uint8_t rs1() const { return (raw >> 15) & 0xF; }
  uint8_t rs2() const { return (raw >> 11) & 0xF; }

  int32_t imm15() const { return sext(raw & 0x7FFF, 15); }
  int32_t imm19() const { return sext(raw & 0x7FFFF, 19); }
  int32_t off23() const { return sext(raw & 0x7FFFFF, 23); }

  // P-type
  uint8_t pd()    const { return (raw >> 21) & 0x3; }
  uint8_t cmp()   const { return (raw >> 18) & 0x7; }
  uint8_t prs1()  const { return (raw >> 14) & 0xF; }
  uint8_t prs2()  const { return (raw >> 10) & 0xF; }
  int32_t imm14() const { return sext(raw & 0x3FFF, 14); }

  static int32_t sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
  }
};

// ---- Golden integer-ALU reference (shared by the ISS and the RTL ALU testbench) ----
// For MOV/MOVI the caller passes the pass-through value in `a`. Non-ALU opcodes return 0.
inline uint32_t alu_ref(uint8_t op, uint32_t a, uint32_t b) {
  switch (op) {
    case OP_ADD: case OP_ADDI: return a + b;
    case OP_SUB:               return a - b;
    case OP_MUL:               return (uint32_t)((int32_t)a * (int32_t)b);
    case OP_MULH:              return (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
    case OP_AND: case OP_ANDI: return a & b;
    case OP_OR:  case OP_ORI:  return a | b;
    case OP_XOR: case OP_XORI: return a ^ b;
    case OP_NOT:               return ~a;
    case OP_SHL: case OP_SHLI: return a << (b & 31);
    case OP_SHR: case OP_SHRI: return a >> (b & 31);
    case OP_SRA: case OP_SRAI: return (uint32_t)((int32_t)a >> (b & 31));
    case OP_SLT: case OP_SLTI: return ((int32_t)a < (int32_t)b) ? 1u : 0u;
    case OP_SLTU:case OP_SLTIU:return (a < b) ? 1u : 0u;
    case OP_MIN:               return ((int32_t)a < (int32_t)b) ? a : b;
    case OP_MAX:               return ((int32_t)a > (int32_t)b) ? a : b;
    case OP_SEQ:               return (a == b) ? 1u : 0u;
    case OP_MOV: case OP_MOVI: return a;
  }
  return 0;
}

// ---- Control decode (shared golden for rtl/compute/decode.sv) ------------------
inline bool op_ialu_r(uint8_t op) { return op >= OP_ADD  && op <= OP_SEQ; }   // 0x10-0x1F
inline bool op_ialu_i(uint8_t op) { return op >= OP_ADDI && op <= OP_SLTIU; } // 0x20-0x28
inline bool op_falu(uint8_t op)   { return op >= OP_FADD && op <= OP_F2I;  }  // 0x35-0x3C

// ================================================================================
// Floating-point reference (M15): binary32, ROUND-TOWARD-ZERO, flush-to-zero
// denormals, Inf/NaN out of scope. These integer algorithms are mirrored EXACTLY by
// rtl/compute/fp_alu.sv — the ISS and the RTL agree bit-for-bit by construction.
// ================================================================================
inline uint32_t fp_mul_ref(uint32_t x, uint32_t z) {
  int sx=x>>31, sz=z>>31, s=sx^sz;
  int ex=(x>>23)&0xFF, ez=(z>>23)&0xFF;
  uint32_t mx=x&0x7FFFFF, mz=z&0x7FFFFF;
  if (ex==0||ez==0) return (uint32_t)s<<31;                 // flush: a*0 = (signed) 0
  uint64_t fx=(1u<<23)|mx, fz=(1u<<23)|mz, prod=fx*fz;
  int e=ex+ez-127; uint32_t mant;
  if (prod & (1ull<<47)) { mant=(uint32_t)((prod>>24)&0x7FFFFF); e+=1; }
  else                     mant=(uint32_t)((prod>>23)&0x7FFFFF);
  return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|mant;
}
inline uint32_t fp_add_ref(uint32_t x, uint32_t z) {
  if (((x>>23)&0xFF)==0) return z;                           // FTZ: 0 + z = z
  if (((z>>23)&0xFF)==0) return x;
  int exx=(x>>23)&0xFF, ezz=(z>>23)&0xFF;
  uint32_t mxx=x&0x7FFFFF, mzz=z&0x7FFFFF;
  bool swap = (ezz>exx)||(ezz==exx && mzz>mxx);
  int ex=swap?ezz:exx, ez=swap?exx:ezz, s=swap?(int)(z>>31):(int)(x>>31), e=ex;
  uint32_t fx=(1u<<23)|(swap?mzz:mxx), fz=(1u<<23)|(swap?mxx:mzz), big=fx;
  int diff=(ex-ez>24)?24:(ex-ez);
  uint32_t aligned=fz>>diff;
  if (((x>>31)&1)==((z>>31)&1)) {
    uint32_t s25=big+aligned;
    if (s25&(1u<<24)) { e+=1; return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|((s25>>1)&0x7FFFFF); }
    return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|(s25&0x7FFFFF);
  } else {
    uint32_t sub=big-aligned;
    if (sub==0) return 0;
    for (int i=0;i<24;i++) if (!(sub&(1u<<23))) { sub<<=1; e-=1; }
    return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|(sub&0x7FFFFF);
  }
}
inline uint32_t fp_sub_ref(uint32_t x, uint32_t z) { return fp_add_ref(x, z ^ 0x80000000u); }
inline uint32_t fp_fma_ref(uint32_t a, uint32_t b, uint32_t c) {
  return fp_add_ref(fp_mul_ref(a, b), c);                    // one multiply then one add
}
// total order key: FTZ-canonicalize ±0, then map to an unsigned-comparable key
inline uint32_t fp_key(uint32_t x) {
  if (((x>>23)&0xFF)==0) x = 0;                              // flush ±0/denormal to +0
  return (x & 0x80000000u) ? ~x : (x | 0x80000000u);
}
inline bool fp_cmp_ref(uint8_t cc, uint32_t a, uint32_t b) {
  uint32_t ka = fp_key(a), kb = fp_key(b);
  switch (cc) {
    case CMP_EQ: return ka == kb;
    case CMP_NE: return ka != kb;
    case CMP_LT: return ka <  kb;
    case CMP_LE: return ka <= kb;
    case CMP_GT: return ka >  kb;
    case CMP_GE: return ka >= kb;
    default:     return false;                               // LTU/GEU reserved for floats
  }
}
inline uint32_t fp_min_ref(uint32_t a, uint32_t b) { return fp_key(a) < fp_key(b) ? a : b; }
inline uint32_t fp_max_ref(uint32_t a, uint32_t b) { return fp_key(a) > fp_key(b) ? a : b; }
inline uint32_t fp_i2f_ref(uint32_t xu) {                    // int32 -> float, truncate
  int32_t x = (int32_t)xu;
  if (x == 0) return 0;
  uint32_t s = (x < 0) ? 1u : 0u;
  uint32_t mag = (x < 0) ? (uint32_t)(-(int64_t)x) : (uint32_t)x;
  int p = 31; while (!(mag & (1u << p))) --p;                // msb position (mag != 0)
  uint32_t mant = (p > 23) ? (mag >> (p - 23)) : (mag << (23 - p));
  uint32_t e = 127 + (uint32_t)p;
  return (s << 31) | ((e & 0xFF) << 23) | (mant & 0x7FFFFF);
}
inline uint32_t fp_f2i_ref(uint32_t x) {                     // float -> int32, trunc, saturate
  uint32_t s = x >> 31, ex = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
  if (ex == 0) return 0;                                     // FTZ
  int e = (int)ex - 127;
  if (e < 0) return 0;
  if (e > 30) return s ? 0x80000000u : 0x7FFFFFFFu;          // overflow saturates
  uint32_t f = (1u << 23) | m;
  uint32_t mag = (e <= 23) ? (f >> (23 - e)) : (f << (e - 23));
  return s ? (uint32_t)(-(int32_t)mag) : mag;
}

// ================================================================================
// SFU reference (M16): approximate special functions on integers, mirrored EXACTLY
// by rtl/compute/sfu.sv. RCP/RSQRT: exact truncated integer division / square root
// (<= ~2 ULP). SIN/COS: quarter-wave 65-entry Q15 LUT + linear interpolation over a
// Q16 fraction-of-turn phase (~1e-4 absolute error; input range documented in ISA.md).
// ================================================================================
inline uint32_t sfu_rcp_ref(uint32_t x) {
  uint32_t s = x >> 31, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
  if (e == 0)   return (s << 31) | 0x7F800000u;              // 1/0 -> Inf pattern
  if (e >= 253) return (s << 31);                            // underflow -> signed 0
  if (m == 0)   return (s << 31) | ((254u - e) << 23);       // exact power of two
  uint64_t f = (1ull << 23) | m;
  uint64_t r = (1ull << 46) / f;                             // in [2^22, 2^23)
  return (s << 31) | ((253u - e) << 23) | ((uint32_t)(r << 1) & 0x7FFFFF);
}
inline uint64_t sfu_isqrt48(uint64_t v) {                    // integer sqrt, 48-bit input
  uint64_t r = 0, b = 1ull << 46;
  for (int i = 0; i < 24; ++i) {
    if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
    else            { r >>= 1; }
    b >>= 2;
  }
  return r;
}
inline uint32_t sfu_sqrt_ref(uint32_t x) {                   // sqrt of a positive float
  uint32_t e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
  if (e == 0) return 0;
  int E = (int)e - 127;
  uint64_t g = (1ull << 23) | m;
  if (E & 1) { g <<= 1; E -= 1; }                            // make the exponent even
  uint64_t q = sfu_isqrt48(g << 23);                         // in [2^23, 2^24)
  return (uint32_t)((127 + E / 2) << 23) | ((uint32_t)q & 0x7FFFFF);
}
inline uint32_t sfu_rsqrt_ref(uint32_t x) {
  if (((x >> 23) & 0xFF) == 0) return 0x7F800000u;           // rsqrt(0) -> Inf pattern
  if (x >> 31) return 0;                                     // negative: out of domain -> 0
  return sfu_rcp_ref(sfu_sqrt_ref(x));
}
static const uint32_t SFU_SIN_LUT[65] = {                    // sin(i/64 * pi/2) in Q15
  0, 804, 1608, 2411, 3212, 4011, 4808, 5602,
  6393, 7180, 7962, 8740, 9512, 10279, 11039, 11793,
  12540, 13279, 14010, 14733, 15447, 16151, 16846, 17531,
  18205, 18868, 19520, 20160, 20788, 21403, 22006, 22595,
  23170, 23732, 24279, 24812, 25330, 25833, 26320, 26791,
  27246, 27684, 28106, 28511, 28899, 29269, 29622, 29957,
  30274, 30572, 30853, 31114, 31357, 31581, 31786, 31972,
  32138, 32286, 32413, 32522, 32610, 32679, 32729, 32758,
  32768
};
inline uint32_t sfu_phase16(uint32_t x) {                    // fraction-of-turn, Q16 (mod 1)
  uint32_t t = fp_mul_ref(x, 0x3E22F983u);                   // x * (1/2pi), RTZ
  uint32_t e = (t >> 23) & 0xFF, m = t & 0x7FFFFF, s = t >> 31;
  if (e == 0) return 0;
  int sh = (int)e - 127 - 7;                                 // align to Q16
  uint32_t p;
  if (sh > 24) p = 0;                                        // range limit (documented)
  else if (sh >= 0) p = (uint32_t)((((uint64_t)((1u<<23)|m)) << sh) & 0xFFFF);
  else if (sh > -24) p = (uint32_t)((((1u<<23)|m) >> (-sh)) & 0xFFFF);
  else p = 0;
  return s ? ((0x10000u - p) & 0xFFFF) : p;
}
inline uint32_t sfu_sinq15(uint32_t phase16) {               // |sin|, Q15 + sign from quadrant
  uint32_t quad = (phase16 >> 14) & 3, idx14 = phase16 & 0x3FFF;
  if (quad == 1 || quad == 3) idx14 = 0x4000 - idx14;        // mirror in quadrants 1/3
  uint32_t v;
  if (idx14 >= 0x4000) v = SFU_SIN_LUT[64];
  else {
    uint32_t i = idx14 >> 8, f8 = idx14 & 0xFF;
    v = SFU_SIN_LUT[i] + (((SFU_SIN_LUT[i+1] - SFU_SIN_LUT[i]) * f8) >> 8);
  }
  return v | ((quad >> 1) << 31);                            // bit31 = negative half
}
inline uint32_t sfu_q15_to_f(uint32_t vq) {                  // Q15 magnitude+sign -> float
  uint32_t s = vq >> 31, v = vq & 0x7FFFFFFF;
  if (v == 0) return 0;
  int p = 15; while (!(v & (1u << p)) && p > 0) --p;         // msb (v <= 32768)
  uint32_t e = 127 + (uint32_t)(p - 15);
  uint32_t mant = (v << (23 - p)) & 0x7FFFFF;
  return (s << 31) | ((e & 0xFF) << 23) | mant;
}
inline uint32_t sfu_sin_ref(uint32_t x) { return sfu_q15_to_f(sfu_sinq15(sfu_phase16(x))); }
inline uint32_t sfu_cos_ref(uint32_t x) {
  return sfu_q15_to_f(sfu_sinq15((sfu_phase16(x) + 0x4000u) & 0xFFFF));
}
inline uint32_t sfu_ref(uint8_t fn, uint32_t x) {
  switch (fn) {
    case MUFU_RCP:   return sfu_rcp_ref(x);
    case MUFU_RSQRT: return sfu_rsqrt_ref(x);
    case MUFU_SIN:   return sfu_sin_ref(x);
    case MUFU_COS:   return sfu_cos_ref(x);
    default:         return 0;
  }
}

struct DecodeInfo {
  uint8_t opcode, psel, rd, rs1, rs2, pd, cmp;
  bool    gneg;
  int32_t imm;          // format-selected, sign-extended
  bool reg_write;       // writes a vector register (rd)
  bool is_alu, is_load, is_store, mem_shared, is_branch, is_ssy, is_setp, is_rdsr;
  bool uses_rs1, uses_rs2;
  bool alu_a_imm, alu_b_imm;   // ALU operand source select
};

inline DecodeInfo decode_ref(uint32_t raw) {
  Insn in(raw);
  uint8_t op = in.opcode();
  DecodeInfo d{};
  d.opcode = op; d.psel = in.psel(); d.gneg = in.gneg();
  d.rd = in.rd(); d.rs1 = in.rs1(); d.rs2 = in.rs2();
  d.pd = in.pd(); d.cmp = in.cmp();

  bool ialu_r = op_ialu_r(op), ialu_i = op_ialu_i(op), falu = op_falu(op);
  d.is_alu   = ialu_r || ialu_i || op == OP_MOVI || op == OP_MOV || falu || op == OP_MUFU;
  d.is_load  = (op == OP_LDG || op == OP_LDS || op == OP_LDC);
  d.is_store = (op == OP_STG || op == OP_STS);
  d.mem_shared = (op == OP_LDS || op == OP_STS);
  d.is_branch = (op == OP_BRA);
  d.is_ssy   = (op == OP_SSY);
  d.is_setp  = (op == OP_ISETP || op == OP_ISETPI || op == OP_FSETP);
  d.is_rdsr  = (op == OP_RDSR);
  d.reg_write = d.is_alu || d.is_rdsr || d.is_load;

  if (ialu_i)                                              d.imm = in.imm15();
  else if (op == OP_MOVI || op == OP_RDSR || op == OP_LDC) d.imm = in.imm19();
  else if (op == OP_LDG || op == OP_LDS || op == OP_STG || op == OP_STS) d.imm = in.imm15();
  else if (op == OP_BRA || op == OP_SSY || op == OP_CALL)  d.imm = in.off23();
  else if (op == OP_ISETPI)                                d.imm = in.imm14();
  else                                                     d.imm = 0;

  d.uses_rs1 = ialu_r || ialu_i || op == OP_MOV || falu || op == OP_MUFU ||
               op == OP_LDG || op == OP_LDS || op == OP_STG || op == OP_STS ||
               op == OP_ATOMG || op == OP_ATOMS;
  d.uses_rs2 = (ialu_r && op != OP_NOT) ||
               (falu && op != OP_I2F && op != OP_F2I) ||
               op == OP_ATOMG || op == OP_ATOMS;
  d.alu_a_imm = (op == OP_MOVI);
  d.alu_b_imm = ialu_i;
  return d;
}

// ---- Encoders (used by unit tests / disassembler; asm.py mirrors these) --------
inline uint32_t enc_guard(uint8_t psel, bool neg) {
  return ((uint32_t)(neg & 1) << 25) | ((uint32_t)(psel & 3) << 23);
}
inline uint32_t enc_r(uint8_t op, uint8_t rd, uint8_t rs1, uint8_t rs2,
                      uint8_t psel = PRED_TRUE, bool neg = false) {
  return ((uint32_t)op << 26) | enc_guard(psel, neg) |
         ((uint32_t)(rd & 0xF) << 19) | ((uint32_t)(rs1 & 0xF) << 15) |
         ((uint32_t)(rs2 & 0xF) << 11);
}
inline uint32_t enc_i(uint8_t op, uint8_t rd, uint8_t rs1, int32_t imm,
                      uint8_t psel = PRED_TRUE, bool neg = false) {
  return ((uint32_t)op << 26) | enc_guard(psel, neg) |
         ((uint32_t)(rd & 0xF) << 19) | ((uint32_t)(rs1 & 0xF) << 15) |
         ((uint32_t)imm & 0x7FFF);
}
inline uint32_t enc_u(uint8_t op, uint8_t rd, int32_t imm,
                      uint8_t psel = PRED_TRUE, bool neg = false) {
  return ((uint32_t)op << 26) | enc_guard(psel, neg) |
         ((uint32_t)(rd & 0xF) << 19) | ((uint32_t)imm & 0x7FFFF);
}
inline uint32_t enc_b(uint8_t op, int32_t off, uint8_t psel = PRED_TRUE, bool neg = false) {
  return ((uint32_t)op << 26) | enc_guard(psel, neg) | ((uint32_t)off & 0x7FFFFF);
}
inline uint32_t enc_p(uint8_t op, uint8_t pd, uint8_t cmp, uint8_t rs1, uint8_t rs2,
                      uint8_t psel = PRED_TRUE, bool neg = false) {
  return ((uint32_t)op << 26) | enc_guard(psel, neg) |
         ((uint32_t)(pd & 3) << 21) | ((uint32_t)(cmp & 7) << 18) |
         ((uint32_t)(rs1 & 0xF) << 14) | ((uint32_t)(rs2 & 0xF) << 10);
}

} // namespace sirion
