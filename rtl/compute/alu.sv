// alu.sv — Sirion integer ALU (combinational, scalar 32-bit).
//
// One lane's worth of integer datapath. The SIMT execute stage (M4) replicates this
// across the 32 lanes of a warp. The caller selects `b` (= rs2 for R-type, imm for
// I-type) and passes the pass-through value in `a` for MOV/MOVI, so this block only
// needs the opcode + two operands — matching the golden `alu_ref()` in sim/iss/isa.hpp.
//
// `is_alu` is high iff the opcode is one this unit handles (lets the execute stage route
// non-ALU ops elsewhere). Purely combinational; no reset/clock.
//
// Conventions: ANSI ports, in-header package import, snake_case.

`default_nettype none

module alu
  import sirion_pkg::*;
(
  input  wire  [OPC_W-1:0] opcode,   // opcode_e
  input  wire  word_t      a,
  input  wire  word_t      b,
  output logic             is_alu,   // 1 => opcode handled here
  output word_t            y
);

  logic [4:0]         shamt;
  logic signed [63:0] sa, sb, sprod; // 64-bit signed product for MULH
  assign shamt = b[4:0];
  assign sa    = $signed(a);         // sign-extended 32 -> 64
  assign sb    = $signed(b);
  assign sprod = sa * sb;

  always_comb begin
    y      = '0;
    is_alu = 1'b1;
    unique case (opcode_e'(opcode))
      OP_ADD,  OP_ADDI  : y = a + b;
      OP_SUB            : y = a - b;
      OP_MUL            : y = a * b;                       // low 32 bits
      OP_MULH           : y = sprod[63:32];                // signed high 32 bits
      OP_AND,  OP_ANDI  : y = a & b;
      OP_OR,   OP_ORI   : y = a | b;
      OP_XOR,  OP_XORI  : y = a ^ b;
      OP_NOT            : y = ~a;
      OP_SHL,  OP_SHLI  : y = a << shamt;
      OP_SHR,  OP_SHRI  : y = a >> shamt;                  // logical
      OP_SRA,  OP_SRAI  : y = $signed(a) >>> shamt;        // arithmetic
      OP_SLT,  OP_SLTI  : y = ($signed(a) <  $signed(b)) ? 32'd1 : 32'd0;
      OP_SLTU, OP_SLTIU : y = (a < b)                     ? 32'd1 : 32'd0;
      OP_MIN            : y = ($signed(a) <  $signed(b)) ? a : b;
      OP_MAX            : y = ($signed(a) >  $signed(b)) ? a : b;
      OP_SEQ            : y = (a == b)                    ? 32'd1 : 32'd0;
      OP_MOV,  OP_MOVI  : y = a;                           // pass-through
      default           : begin y = '0; is_alu = 1'b0; end
    endcase
  end

  // ----------------------------------------------------------------------------
  // Assertions (SVA)
  // ----------------------------------------------------------------------------
`ifndef SYNTHESIS
  // A handled ALU op never produces X on its result (operands assumed known).
  always_comb begin
    if (is_alu && !$isunknown({a, b}))
      a_no_x_when_alu: assert (!$isunknown(y));
  end
`endif

endmodule

`default_nettype wire
