// cta_dispatch.sv — workgroup (CTA/block) dispatcher (M12, multi-CU M17).
//
// Walks the launch grid and hands each block to whichever Compute Unit is FREE, so blocks
// execute concurrently across the CU array (occupancy tracking = one busy bit per CU).
// When every block has been dispatched and every CU has drained, it invalidates all L1s
// and flushes the shared L2, then pulses grid_done — so grid completion IS the memory-
// coherence point for the host (and for the next launch).
//
// Per-CU context: each CU keeps its own latched ctx_bidx (driven here at start time);
// tpb/entry are common to the launch.
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module cta_dispatch
  import sirion_pkg::*;
#(
  parameter int IMEM_WORDS = 256,
  parameter int NUM_CU     = 1
) (
  input  wire                          clk,
  input  wire                          rst,
  // launch descriptor (latched on `launch`)
  input  wire                          launch,
  input  wire  word_t                  grid_nx,   // blocks in the grid (1D)
  input  wire  word_t                  tpb,       // threads per block
  input  wire [$clog2(IMEM_WORDS)-1:0] entry,     // kernel entry PC (word)
  // CU control (one start/bidx per CU; tpb/entry shared)
  output logic [NUM_CU-1:0]            cu_start,
  output word_t                        cu_bidx [NUM_CU],
  output word_t                        cu_tpb,
  output logic [$clog2(IMEM_WORDS)-1:0] cu_entry,
  input  wire  [NUM_CU-1:0]            cu_busy,
  input  wire  [NUM_CU-1:0]            cu_done,
  // grid-end cache maintenance
  output logic [NUM_CU-1:0]            cu_flush,       // invalidate each CU's L1
  input  wire  [NUM_CU-1:0]            cu_flush_done,
  output logic                         l2_flush,       // write back + invalidate the L2
  input  wire                          l2_flush_done,
  // status
  output logic                         grid_busy,
  output logic                         grid_done, // 1-cycle pulse when the grid completes
  // perf
  output logic [31:0]                  perf_blocks   // blocks dispatched since reset
);

  typedef enum logic [2:0] {D_IDLE, D_DISPATCH, D_DRAIN, D_FLUSH, D_FWAIT} dstate_e;
  dstate_e dstate;

  word_t nx_q, next_bidx;
  logic [NUM_CU-1:0] launched;    // CUs that have been started at least once this grid
  logic [NUM_CU-1:0] flush_seen;  // per-CU flush_done collector

  assign grid_busy = (dstate != D_IDLE);

  // a CU is free if it has never been launched this grid, or it is done with its block
  logic [NUM_CU-1:0] cu_free;
  always_comb
    for (int c = 0; c < NUM_CU; c++)
      cu_free[c] = !launched[c] || cu_done[c];

  // first free CU (priority encoder — fairness comes from blocks finishing at random)
  localparam int FCW = (NUM_CU > 1) ? $clog2(NUM_CU) : 1;
  logic           have_free;
  logic [FCW-1:0] free_cu;
  always_comb begin
    have_free = 1'b0; free_cu = '0;
    for (int c = NUM_CU - 1; c >= 0; c--)
      if (cu_free[c] && !cu_start[c]) begin have_free = 1'b1; free_cu = FCW'(c); end
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      dstate <= D_IDLE; cu_start <= '0; grid_done <= 1'b0;
      cu_flush <= '0; l2_flush <= 1'b0;
      cu_tpb <= '0; cu_entry <= '0; nx_q <= '0; next_bidx <= '0;
      launched <= '0; flush_seen <= '0; perf_blocks <= '0;
      for (int c = 0; c < NUM_CU; c++) cu_bidx[c] <= '0;
    end else begin
      cu_start  <= '0;
      grid_done <= 1'b0;
      cu_flush  <= '0;
      l2_flush  <= 1'b0;
      unique case (dstate)
        D_IDLE: if (launch) begin
          nx_q      <= grid_nx;
          next_bidx <= '0;
          cu_tpb    <= tpb;
          cu_entry  <= entry;
          launched  <= '0;
          if (grid_nx == 0) grid_done <= 1'b1;         // empty grid completes immediately
          else              dstate    <= D_DISPATCH;
        end

        D_DISPATCH: begin
          // hand the next block to a free CU (one start per cycle)
          if (have_free) begin
            cu_start[free_cu]  <= 1'b1;
            cu_bidx[free_cu]   <= next_bidx;
            launched[free_cu]  <= 1'b1;
            perf_blocks        <= perf_blocks + 1;
            next_bidx          <= next_bidx + 1;
            if (next_bidx + 1 >= nx_q) dstate <= D_DRAIN;
          end
        end

        D_DRAIN: begin
          // all blocks dispatched: wait until every launched CU reports done.
          // (a start pulse takes a cycle to land, so also require no start in flight)
          if (((~launched) | cu_done) == {NUM_CU{1'b1}} && cu_start == '0) begin
            cu_flush   <= {NUM_CU{1'b1}};              // invalidate every L1
            l2_flush   <= 1'b1;                        // write back + invalidate the L2
            flush_seen <= '0;
            dstate     <= D_FLUSH;
          end
        end

        D_FLUSH: dstate <= D_FWAIT;                    // flush pulses issued last cycle

        D_FWAIT: begin
          flush_seen <= flush_seen | cu_flush_done;
          if (((flush_seen | cu_flush_done) == {NUM_CU{1'b1}}) && l2_flush_done) begin
            grid_done <= 1'b1;
            dstate    <= D_IDLE;
          end
        end
      endcase
    end
  end

`ifndef SYNTHESIS
  a_start_busy: assert property (@(posedge clk) disable iff (rst) (|cu_start) |-> grid_busy);
`endif

endmodule

`default_nettype wire
