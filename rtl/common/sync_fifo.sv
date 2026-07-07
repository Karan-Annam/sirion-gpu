// sync_fifo.sv — synchronous circular FIFO (reusable primitive, M2).
//
// Standard (non-FWFT) FIFO: `dout` combinationally reflects the head element and is valid
// whenever !empty; `pop` advances the head. `push` writes when !full. DEPTH must be a
// power of two (pointers wrap naturally). Used later for issue/memory request queues.
//
// Conventions: ANSI ports, synchronous active-high reset, snake_case.

`default_nettype none

module sync_fifo #(
  parameter int WIDTH = 32,
  parameter int DEPTH = 8
) (
  input  wire              clk,
  input  wire              rst,
  input  wire              push,
  input  wire  [WIDTH-1:0] din,
  output logic             full,
  input  wire              pop,
  output logic [WIDTH-1:0] dout,
  output logic             empty,
  output logic [$clog2(DEPTH+1)-1:0] count
);

  localparam int PW = $clog2(DEPTH);

  logic [WIDTH-1:0] mem [DEPTH];
  logic [PW-1:0]    wptr, rptr;

  assign empty = (count == '0);
  assign full  = (count == DEPTH[$clog2(DEPTH+1)-1:0]);
  assign dout  = mem[rptr];

  wire do_push = push && !full;
  wire do_pop  = pop  && !empty;

  always_ff @(posedge clk) begin
    if (rst) begin
      wptr  <= '0;
      rptr  <= '0;
      count <= '0;
    end else begin
      if (do_push) begin
        mem[wptr] <= din;
        wptr      <= wptr + 1'b1;   // wraps (DEPTH is a power of two)
      end
      if (do_pop) rptr <= rptr + 1'b1;
      count <= count + (do_push ? 1 : 0) - (do_pop ? 1 : 0);
    end
  end

  // ----------------------------------------------------------------------------
  // Assertions (SVA)
  // ----------------------------------------------------------------------------
`ifndef SYNTHESIS
  a_not_full_empty: assert property (@(posedge clk) disable iff (rst) !(full && empty));
  a_count_range:    assert property (@(posedge clk) disable iff (rst) count <= DEPTH);
  a_no_overflow:    assert property (@(posedge clk) disable iff (rst) (full  |-> !do_push));
  a_no_underflow:   assert property (@(posedge clk) disable iff (rst) (empty |-> !do_pop));
`endif

endmodule

`default_nettype wire
