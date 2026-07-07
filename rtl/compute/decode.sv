// decode.sv — Sirion instruction decoder (combinational) — M2.
//
// Splits a 32-bit instruction into register/immediate fields and control signals for the
// issue/execute stages. Mirrors the golden decode_ref() in sim/iss/isa.hpp exactly; the
// unit test enforces field-for-field agreement over random instructions.
//
// Conventions: ANSI ports, in-header import, snake_case, combinational.

`default_nettype none

module decode
  import sirion_pkg::*;
(
  input  wire  insn_t         insn,
  // decoded fields
  output logic [OPC_W-1:0]    opcode,
  output logic                gneg,
  output logic [1:0]          psel,
  output vreg_id_t            rd,
  output vreg_id_t            rs1,
  output vreg_id_t            rs2,
  output logic [1:0]          pd,
  output logic [2:0]          cmp,
  output word_t               imm,        // format-selected, sign-extended
  // control
  output logic                reg_write,
  output logic                is_alu,
  output logic                is_load,
  output logic                is_store,
  output logic                mem_shared,
  output logic                is_branch,
  output logic                is_ssy,
  output logic                is_setp,
  output logic                is_rdsr,
  output logic                uses_rs1,
  output logic                uses_rs2,
  output logic                alu_a_imm,
  output logic                alu_b_imm
);

  logic [5:0] op;
  assign op = insn[31:26];

  assign opcode = op;
  assign gneg   = insn_gneg(insn);
  assign psel   = insn_psel(insn);
  assign rd     = insn_rd(insn);
  assign rs1    = insn_rs1(insn);
  assign rs2    = insn_rs2(insn);
  assign pd     = insn_pd(insn);
  assign cmp    = insn_cmp(insn);

  // opcode ranges (hex to avoid enum relational operators)
  logic ialu_r, ialu_i, falu;
  assign ialu_r = (op >= 6'h10) && (op <= 6'h1F);   // ADD..SEQ
  assign ialu_i = (op >= 6'h20) && (op <= 6'h28);   // ADDI..SLTIU
  assign falu   = (op >= 6'h35) && (op <= 6'h3C);   // FADD..F2I (M15)

  assign is_alu     = ialu_r || ialu_i || (op == OP_MOVI) || (op == OP_MOV) || falu ||
                      (op == OP_MUFU);
  assign is_load    = (op == OP_LDG) || (op == OP_LDS) || (op == OP_LDC);
  assign is_store   = (op == OP_STG) || (op == OP_STS);
  assign mem_shared = (op == OP_LDS) || (op == OP_STS);
  assign is_branch  = (op == OP_BRA);
  assign is_ssy     = (op == OP_SSY);
  assign is_setp    = (op == OP_ISETP) || (op == OP_ISETPI) || (op == OP_FSETP);
  assign is_rdsr    = (op == OP_RDSR);
  assign reg_write  = is_alu || is_rdsr || is_load;
  assign uses_rs1   = ialu_r || ialu_i || (op == OP_MOV) || falu || (op == OP_MUFU) ||
                      (op == OP_LDG) || (op == OP_LDS) || (op == OP_STG) || (op == OP_STS) ||
                      (op == OP_ATOMG) || (op == OP_ATOMS);
  assign uses_rs2   = (ialu_r && (op != OP_NOT)) ||
                      (falu && (op != OP_I2F) && (op != OP_F2I)) ||
                      (op == OP_ATOMG) || (op == OP_ATOMS);
  assign alu_a_imm  = (op == OP_MOVI);
  assign alu_b_imm  = ialu_i;

  // immediate selection by format
  always_comb begin
    if (ialu_i)
      imm = insn_imm15(insn);
    else if ((op == OP_MOVI) || (op == OP_RDSR) || (op == OP_LDC))
      imm = insn_imm19(insn);
    else if ((op == OP_LDG) || (op == OP_LDS) || (op == OP_STG) || (op == OP_STS))
      imm = insn_imm15(insn);
    else if ((op == OP_BRA) || (op == OP_SSY) || (op == OP_CALL))
      imm = insn_off23(insn);
    else if (op == OP_ISETPI)
      imm = insn_imm14(insn);
    else
      imm = '0;
  end

  // ----------------------------------------------------------------------------
  // Assertions (SVA) — class flags are mutually exclusive.
  // ----------------------------------------------------------------------------
`ifndef SYNTHESIS
  always_comb begin
    if (!$isunknown(insn)) begin
      a_excl_alu_mem:    assert (!(is_alu && (is_load || is_store)));
      a_excl_load_store: assert (!(is_load && is_store));
      a_excl_alu_branch: assert (!(is_alu && is_branch));
    end
  end
`endif

endmodule

`default_nettype wire
