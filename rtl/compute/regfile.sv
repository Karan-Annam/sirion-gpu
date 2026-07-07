// regfile.sv — Sirion vector register file (M2; per-warp banked in M11).
//
// Holds NUM_VREGS registers PER RESIDENT WARP, each a full warp of lane words
// (WARP_SIZE x XLEN). One predicated write port (per-lane write mask, for masked SIMT
// writeback) and two registered read ports (1-cycle latency, like a real RF/BRAM). Reads
// have no write bypass — a concurrent read+write to the same register returns the OLD
// value; the execute stage forwards when needed (M3).
//
// Multi-warp (M11): storage is regs[NUM_WARPS][NUM_VREGS]. M13 (barrel pipeline): the READ
// and EXEC stages hold DIFFERENT warps in the same cycle, so reads take their own warp
// selector (`rwarp`) and each write port carries its own (`wwarp`, `wwarp2`). Port 2 exists
// because an ALU writeback (EXEC stage) and a load writeback (memory unit) can complete in
// the same cycle — always for different warps (one instruction per warp in flight), so the
// two ports never collide on the same storage word.
//
// Banking: registers are assigned to NUM_BANKS banks by (reg_id % NUM_BANKS). Storage
// here is flip-flop based and always returns correct data on both ports; `bank_conflict`
// flags when the two read addresses hit the same bank so the operand collector can
// serialize once we move to single-ported BRAM banks. This module models storage +
// surfaces the conflict signal; it does not itself serialize.
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module regfile
  import sirion_pkg::*;
#(
  parameter  int NUM_BANKS = 4,
  parameter  int NUM_WARPS = 1,
  localparam int WW = (NUM_WARPS > 1) ? $clog2(NUM_WARPS) : 1
) (
  input  wire                 clk,
  input  wire                 rst,
  // warp selector for the two read ports
  input  wire  [WW-1:0]       rwarp,
  // predicated write port 1 (EXEC-stage ALU/RDSR/LDC writeback)
  input  wire                 we,
  input  wire  [WW-1:0]       wwarp,
  input  wire  vreg_id_t      waddr,
  input  wire  warp_vec_t     wdata,
  input  wire  warp_mask_t    wmask,
  // predicated write port 2 (memory-unit load writeback; different warp than port 1)
  input  wire                 we2,
  input  wire  [WW-1:0]       wwarp2,
  input  wire  vreg_id_t      waddr2,
  input  wire  warp_vec_t     wdata2,
  input  wire  warp_mask_t    wmask2,
  // three read ports (registered outputs; port 2 feeds the FFMA accumulator, M15)
  input  wire                 ren0,
  input  wire  vreg_id_t      raddr0,
  input  wire                 ren1,
  input  wire  vreg_id_t      raddr1,
  input  wire                 ren2,
  input  wire  vreg_id_t      raddr2,
  output warp_vec_t           rdata0,
  output warp_vec_t           rdata1,
  output warp_vec_t           rdata2,
  output logic                bank_conflict
);

  localparam int BW = (NUM_BANKS > 1) ? $clog2(NUM_BANKS) : 1;

  warp_vec_t regs [NUM_WARPS][NUM_VREGS];

  // ---- writes (per-lane masked; the two ports target different warps by construction) ----
  always_ff @(posedge clk) begin
    if (rst) begin
      for (int w = 0; w < NUM_WARPS; w++)
        for (int r = 0; r < NUM_VREGS; r++) regs[w][r] <= '0;
    end else begin
      if (we)
        for (int l = 0; l < WARP_SIZE; l++)
          if (wmask[l]) regs[wwarp][waddr][l] <= wdata[l];
      if (we2)
        for (int l = 0; l < WARP_SIZE; l++)
          if (wmask2[l]) regs[wwarp2][waddr2][l] <= wdata2[l];
    end
  end

  // ---- registered reads (no bypass) ----
  always_ff @(posedge clk) begin
    if (rst) begin
      rdata0 <= '0;
      rdata1 <= '0;
      rdata2 <= '0;
    end else begin
      if (ren0) rdata0 <= regs[rwarp][raddr0];
      if (ren1) rdata1 <= regs[rwarp][raddr1];
      if (ren2) rdata2 <= regs[rwarp][raddr2];
    end
  end

  // ---- bank-conflict detection (combinational; consumed by the operand collector) ----
  logic [BW-1:0] bank0, bank1;
  assign bank0 = raddr0[BW-1:0];
  assign bank1 = raddr1[BW-1:0];
  assign bank_conflict = ren0 && ren1 && (bank0 == bank1) && (raddr0 != raddr1);

  // ----------------------------------------------------------------------------
  // Assertions (SVA)
  // ----------------------------------------------------------------------------
`ifndef SYNTHESIS
  // Conflict is reported exactly when two enabled reads share a bank but differ.
  a_conflict_def: assert property (@(posedge clk) disable iff (rst)
    bank_conflict == (ren0 && ren1 && (bank0 == bank1) && (raddr0 != raddr1)));
  // The two write ports never target the same warp in the same cycle (one instruction
  // per warp in flight makes this impossible by construction).
  a_wports_disjoint: assert property (@(posedge clk) disable iff (rst)
    !(we && we2 && (wwarp == wwarp2)));
  // Read data is known after reset (storage is reset to 0, so no X escapes).
  a_rd0_known: assert property (@(posedge clk) disable iff (rst) !$isunknown(rdata0));
  a_rd1_known: assert property (@(posedge clk) disable iff (rst) !$isunknown(rdata1));
`endif

endmodule

`default_nettype wire
