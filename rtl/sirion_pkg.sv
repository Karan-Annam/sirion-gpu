// sirion_pkg.sv — Sirion GPU global parameters, types, and ISA encodings (spec §2, §5).
//
// This is the single machine-readable source of truth shared by the RTL, the C++
// instruction-set simulator (sim/iss), and the Python assembler (scripts/asm.py).
// Anything that describes the ISA or core sizing lives here so the three stay in lockstep.
//
// M0 status: sizing params + core typedefs are final; the opcode enum below is a small
// placeholder used only to make the package self-consistent. The complete ISA opcode map
// is defined in M1 alongside docs/ISA.md and must match it exactly.
//
// Style: ANSI/in-header imports elsewhere; sync active-high reset; snake_case; UPPER_CASE
// enum members with a type prefix; one package per project (guarded).

`ifndef SIRION_PKG_SV
`define SIRION_PKG_SV

package sirion_pkg;

  // ----------------------------------------------------------------------------
  // Core machine sizing (final)
  // ----------------------------------------------------------------------------
  parameter int WARP_SIZE   = 32;              // threads per warp (SIMT lanes)
  parameter int LANE_ID_W   = $clog2(WARP_SIZE); // 5

  parameter int XLEN        = 32;              // data path / register width (bits)

  parameter int NUM_VREGS   = 16;              // vector GPRs per thread: R0..R15
  parameter int VREG_ADDR_W = $clog2(NUM_VREGS); // 4

  parameter int NUM_SREGS   = 16;              // scalar/uniform registers: SR0..SR15
  parameter int SREG_ADDR_W = $clog2(NUM_SREGS); // 4

  parameter int NUM_PREDS   = 4;               // predicate registers P0..P3 (P0 == PT, true)
  parameter int PRED_SEL_W  = $clog2(NUM_PREDS); // 2

  parameter int INSN_W      = 32;              // fixed-width instruction encoding
  parameter int OPC_W       = 6;               // opcode field width  -> 64 opcodes
  parameter int GUARD_W     = 3;               // predicate guard: {negate, predsel[1:0]}

  parameter int PC_W        = 32;              // program counter width (byte address)
  parameter int MEM_ADDR_W  = 32;              // flat global address width

  // Predicate register 0 is architecturally hardwired TRUE (the "PT" predicate).
  parameter int PRED_TRUE   = 0;

  // ----------------------------------------------------------------------------
  // Core typedefs (final)
  // ----------------------------------------------------------------------------
  typedef logic [XLEN-1:0]        word_t;        // one 32-bit scalar/lane word
  typedef logic [WARP_SIZE-1:0]   warp_mask_t;   // per-lane active/predicate mask
  typedef logic [WARP_SIZE-1:0][XLEN-1:0] warp_vec_t; // a full warp of lane words (packed)
  typedef logic [INSN_W-1:0]      insn_t;        // a raw instruction word
  typedef logic [PC_W-1:0]        pc_t;
  typedef logic [MEM_ADDR_W-1:0]  addr_t;
  typedef logic [VREG_ADDR_W-1:0] vreg_id_t;
  typedef logic [OPC_W-1:0]       opcode_t;

  // ----------------------------------------------------------------------------
  // Opcode map (ISA v1 — FINAL; mirrors sim/iss/isa.hpp and docs/ISA.md exactly)
  // ----------------------------------------------------------------------------
  typedef enum logic [OPC_W-1:0] {
    // control / misc (0x00-0x0F)
    OP_NOP  = 6'h00, OP_EXIT = 6'h01, OP_BAR = 6'h02, OP_BRA  = 6'h03,
    OP_SSY  = 6'h04, OP_SYNC = 6'h05, OP_CALL = 6'h06, OP_RET = 6'h07, OP_RDSR = 6'h08,
    // integer ALU, register-register (0x10-0x1F)
    OP_ADD  = 6'h10, OP_SUB = 6'h11, OP_MUL = 6'h12, OP_MULH = 6'h13,
    OP_AND  = 6'h14, OP_OR  = 6'h15, OP_XOR = 6'h16, OP_NOT  = 6'h17,
    OP_SHL  = 6'h18, OP_SHR = 6'h19, OP_SRA = 6'h1A, OP_SLT  = 6'h1B,
    OP_SLTU = 6'h1C, OP_MIN = 6'h1D, OP_MAX = 6'h1E, OP_SEQ  = 6'h1F,
    // integer ALU, immediate + moves + predicate-set (0x20-0x2C)
    OP_ADDI = 6'h20, OP_ANDI = 6'h21, OP_ORI = 6'h22, OP_XORI = 6'h23,
    OP_SHLI = 6'h24, OP_SHRI = 6'h25, OP_SRAI = 6'h26, OP_SLTI = 6'h27,
    OP_SLTIU= 6'h28, OP_MOVI = 6'h29, OP_MOV = 6'h2A, OP_ISETP = 6'h2B, OP_ISETPI = 6'h2C,
    // memory (0x30-0x34)
    OP_LDG  = 6'h30, OP_STG = 6'h31, OP_LDS = 6'h32, OP_STS = 6'h33, OP_LDC = 6'h34,
    // floating point, binary32 (0x35-0x3D) — M15. RTZ, FTZ; mirrors isa.hpp fp_*_ref.
    OP_FADD = 6'h35, OP_FSUB = 6'h36, OP_FMUL = 6'h37, OP_FFMA = 6'h38, // FFMA: rd=rs1*rs2+rd
    OP_FMIN = 6'h39, OP_FMAX = 6'h3A, OP_I2F  = 6'h3B, OP_F2I  = 6'h3C,
    OP_FSETP= 6'h3D,                                                    // float ISETP
    // M16: SFU + atomics
    OP_MUFU = 6'h2D,  // MUFU.func rd, rs1 — func in insn[2:0] (RCP/RSQRT/SIN/COS)
    OP_ATOMG= 6'h3E,  // ATOMG.func rd, [rs1], rs2 — global atomic RMW, rd <- old
    OP_ATOMS= 6'h3F   //   ATOMS: shared memory. func in insn[10:8]; CAS compare = rd
  } opcode_e;

  // MUFU function codes (insn[2:0]) and atomic function codes (insn[10:8]).
  localparam logic [2:0] MUFU_RCP = 3'd0, MUFU_RSQRT = 3'd1, MUFU_SIN = 3'd2, MUFU_COS = 3'd3;
  localparam logic [2:0] ATOM_ADD = 3'd0, ATOM_MIN = 3'd1, ATOM_MAX = 3'd2,
                         ATOM_EXCH = 3'd3, ATOM_CAS = 3'd4;

  // ISETP comparison codes (3 bits).
  localparam logic [2:0] CMP_EQ = 3'd0, CMP_NE = 3'd1, CMP_LT = 3'd2, CMP_LE = 3'd3,
                         CMP_GT = 3'd4, CMP_GE = 3'd5, CMP_LTU = 3'd6, CMP_GEU = 3'd7;

  // Special-register indices (RDSR rd, #srid).
  localparam int SR_TID_X = 0,  SR_TID_Y = 1,  SR_TID_Z = 2,
                 SR_NTID_X = 3, SR_NTID_Y = 4, SR_NTID_Z = 5,
                 SR_CTAID_X = 6, SR_CTAID_Y = 7, SR_CTAID_Z = 8,
                 SR_NCTAID_X = 9, SR_NCTAID_Y = 10, SR_NCTAID_Z = 11,
                 SR_LANEID = 12, SR_WARPID = 13, SR_TID_FLAT = 14;

  // ----------------------------------------------------------------------------
  // Instruction-field extractors (used by decode.sv; mirror Insn in isa.hpp)
  //   [31:26] opcode  [25] guard.neg  [24:23] guard.psel
  //   R: [22:19]rd [18:15]rs1 [14:11]rs2   I: rd rs1 [14:0]imm15
  //   U: rd [18:0]imm19   B: [22:0]off23   ST: rd=data rs1=base imm15
  //   P(ISETP): [22:21]pd [20:18]cmp [17:14]rs1 [13:10]rs2   Pi: ...[13:0]imm14
  // ----------------------------------------------------------------------------
  function automatic opcode_e         insn_opcode(insn_t i); return opcode_e'(i[31:26]); endfunction
  function automatic logic            insn_gneg  (insn_t i); return i[25];               endfunction
  function automatic logic [1:0]      insn_psel  (insn_t i); return i[24:23];            endfunction
  function automatic vreg_id_t        insn_rd    (insn_t i); return i[22:19];            endfunction
  function automatic vreg_id_t        insn_rs1   (insn_t i); return i[18:15];            endfunction
  function automatic vreg_id_t        insn_rs2   (insn_t i); return i[14:11];            endfunction
  function automatic word_t           insn_imm15 (insn_t i); return {{(XLEN-15){i[14]}}, i[14:0]}; endfunction
  function automatic word_t           insn_imm19 (insn_t i); return {{(XLEN-19){i[18]}}, i[18:0]}; endfunction
  function automatic word_t           insn_off23 (insn_t i); return {{(XLEN-23){i[22]}}, i[22:0]}; endfunction
  function automatic logic [1:0]      insn_pd    (insn_t i); return i[22:21];            endfunction
  function automatic logic [2:0]      insn_cmp   (insn_t i); return i[20:18];            endfunction
  function automatic vreg_id_t        insn_prs1  (insn_t i); return i[17:14];            endfunction
  function automatic vreg_id_t        insn_prs2  (insn_t i); return i[13:10];            endfunction
  function automatic word_t           insn_imm14 (insn_t i); return {{(XLEN-14){i[13]}}, i[13:0]}; endfunction

endpackage : sirion_pkg

`endif // SIRION_PKG_SV
