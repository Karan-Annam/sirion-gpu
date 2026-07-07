// scoreboard.sv — per-register in-flight-write tracker for one warp (M2).
//
// Tracks which vector registers have an outstanding (issued-but-not-written-back) write.
// The issue stage presents a candidate instruction; `stall` is asserted combinationally
// if any needed register is busy:
//   * RAW: a source register (rs1/rs2) has a pending write, or
//   * WAW: the destination register has a pending write.
// A single busy bit per register suffices because WAW stalls prevent a second in-flight
// write to the same register. `issue` commits the candidate (sets busy[rd]); `wb_valid`
// clears busy[wb_rd] at writeback. If a set and clear hit the same register in one cycle,
// the set wins (that situation is otherwise precluded by the stall).
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module scoreboard
  import sirion_pkg::*;
(
  input  wire                 clk,
  input  wire                 rst,
  // issue-check port (combinational hazard check)
  input  wire                 check_valid,
  input  wire  vreg_id_t      chk_rs1,
  input  wire  vreg_id_t      chk_rs2,
  input  wire  vreg_id_t      chk_rd,
  input  wire                 chk_uses_rs1,
  input  wire                 chk_uses_rs2,
  input  wire                 chk_writes_rd,
  output logic                stall,
  // commit the issue (set busy[chk_rd]); asserted by issue logic only when !stall
  input  wire                 issue,
  // writeback port (clear busy[wb_rd])
  input  wire                 wb_valid,
  input  wire  vreg_id_t      wb_rd,
  // observability
  output logic [NUM_VREGS-1:0] busy_vec
);

  logic [NUM_VREGS-1:0] busy;
  assign busy_vec = busy;

  assign stall = check_valid &&
                 ((chk_uses_rs1  && busy[chk_rs1]) ||
                  (chk_uses_rs2  && busy[chk_rs2]) ||
                  (chk_writes_rd && busy[chk_rd]));

  always_ff @(posedge clk) begin
    if (rst) begin
      busy <= '0;
    end else begin
      if (wb_valid)               busy[wb_rd]  <= 1'b0;   // clear first
      if (issue && chk_writes_rd) busy[chk_rd] <= 1'b1;   // set wins on same-reg tie
    end
  end

  // ----------------------------------------------------------------------------
  // Assertions (SVA)
  // ----------------------------------------------------------------------------
`ifndef SYNTHESIS
  // A stall must be justified by an actually-busy needed register.
  a_stall_justified: assert property (@(posedge clk) disable iff (rst)
    stall |-> ((chk_uses_rs1 && busy[chk_rs1]) ||
               (chk_uses_rs2 && busy[chk_rs2]) ||
               (chk_writes_rd && busy[chk_rd])));
  // Never commit a write to a register that is already busy (would be a lost WAW).
  a_no_double_issue: assert property (@(posedge clk) disable iff (rst)
    (issue && chk_writes_rd && !(wb_valid && wb_rd == chk_rd)) |-> !busy[chk_rd]);
  a_busy_known: assert property (@(posedge clk) disable iff (rst) !$isunknown(busy));
`endif

endmodule

`default_nettype wire
