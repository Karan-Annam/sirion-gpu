// counter.sv — parametric saturating-wrap up-counter (M0 toolchain-proof module).
//
// Purpose: the smallest useful synchronous module, used only to validate the whole
// build/sim/trace/self-checking-harness chain end-to-end before real RTL is written.
// It is nonetheless a clean, reusable primitive (loadable, enable-gated, wrap tick).
//
// Conventions (project-wide): ANSI ports; synchronous, active-high reset `rst`;
// registered outputs; `tick` is an explicit 1-cycle pulse; snake_case.

`default_nettype none

module counter #(
  parameter int WIDTH = 8
) (
  input  wire              clk,
  input  wire              rst,        // synchronous, active-high
  input  wire              en,         // count enable
  input  wire              load,       // synchronous load (takes priority over en)
  input  wire [WIDTH-1:0]  load_val,
  output logic [WIDTH-1:0] count,
  output logic             tick        // 1-cycle pulse on wrap (max -> 0 via en)
);

  localparam logic [WIDTH-1:0] MAXVAL = {WIDTH{1'b1}};

  always_ff @(posedge clk) begin
    if (rst) begin
      count <= '0;
      tick  <= 1'b0;
    end else if (load) begin
      count <= load_val;
      tick  <= 1'b0;
    end else if (en) begin
      count <= count + 1'b1;
      tick  <= (count == MAXVAL);   // pulses on the cycle the counter wraps to 0
    end else begin
      tick  <= 1'b0;
    end
  end

  // ----------------------------------------------------------------------------
  // Assertions (SVA) — enabled under Verilator with --assert.
  // ----------------------------------------------------------------------------
`ifndef SYNTHESIS
  // tick may only be high the cycle after a wrap-around increment.
  property p_tick_only_on_wrap;
    @(posedge clk) disable iff (rst)
      tick |-> $past(en) && ($past(count) == MAXVAL) && (count == '0);
  endproperty
  a_tick_only_on_wrap: assert property (p_tick_only_on_wrap);

  // count is never X after reset deasserts.
  a_count_known: assert property (@(posedge clk) disable iff (rst) !$isunknown(count));
`endif

endmodule

`default_nettype wire
