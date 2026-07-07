// warp_sched.sv — round-robin warp scheduler (M7).
//
// Picks one ready warp to issue each cycle, rotating fairly so no ready warp is starved.
// This is the arbitration a multi-warp Compute Unit uses to hide latency (issue from another
// ready warp while one is stalled on memory). Combinational selection; `advance` moves the
// round-robin pointer past the chosen warp.
//
// NUM_WARPS must be a power of two (pointer arithmetic wraps by truncation).
//
// Conventions: ANSI ports, synchronous active-high reset.

`default_nettype none

module warp_sched #(
  parameter int NUM_WARPS = 8
) (
  input  wire                        clk,
  input  wire                        rst,
  input  wire [NUM_WARPS-1:0]        ready,    // warps eligible to issue
  input  wire                        advance,  // consume the selection (rotate the pointer)
  output logic                       valid,    // some ready warp exists
  output logic [$clog2(NUM_WARPS)-1:0] sel     // chosen warp
);

  localparam int WW = $clog2(NUM_WARPS);
  logic [WW-1:0] ptr;                            // last-served + priority base

  // Round-robin: scan warps starting just after `ptr`, pick the first ready one.
  always_comb begin
    valid = 1'b0;
    sel   = '0;
    for (int i = 1; i <= NUM_WARPS; i++) begin
      logic [WW-1:0] w;
      w = ptr + i[WW-1:0];                        // wraps mod NUM_WARPS
      if (!valid && ready[w]) begin
        sel   = w;
        valid = 1'b1;
      end
    end
  end

  always_ff @(posedge clk) begin
    if (rst)              ptr <= '0;
    else if (advance && valid) ptr <= sel;        // next cycle, priority is just after sel
  end

`ifndef SYNTHESIS
  // If any warp is ready, one must be selected; and the selected warp must be ready.
  a_valid_iff_ready: assert property (@(posedge clk) disable iff (rst) valid == (|ready));
  a_sel_ready:       assert property (@(posedge clk) disable iff (rst) valid |-> ready[sel]);
`endif

endmodule

`default_nettype wire
