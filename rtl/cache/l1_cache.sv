// l1_cache.sv — direct-mapped, write-back, write-allocate L1 data cache (M7).
//
// A performance layer for the memory system: keeps recently-used lines on-chip so repeated
// accesses hit instead of going to the (slow) backing memory. Verified for *transparency*
// (it returns exactly what a flat memory would) plus correct hit/miss + write-back behavior.
//
// Protocol: pulse `req_valid` for one cycle to start an access; the request is latched, so
// it may drop immediately. `resp_ready` pulses when the access completes (with rdata + hit).
// On a miss it writes back the old line if dirty, fetches the new line, then serves. The
// backing memory is a line-granular combinational read + synchronous write (next level / TB).
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module l1_cache
  import sirion_pkg::*;
#(
  parameter int LINE_WORDS = 4,
  parameter int SETS       = 64
) (
  input  wire                     clk,
  input  wire                     rst,
  // CPU side (one in-flight access; req_valid is a 1-cycle pulse)
  input  wire                     req_valid,
  input  wire                     req_we,
  input  wire  [31:0]             req_addr,
  input  wire  word_t             req_wdata,
  output logic                    resp_ready,
  output word_t                   resp_rdata,
  output logic                    resp_hit,
  // backing memory (line granular): combinational read, synchronous write
  output logic [31:0]             mem_raddr,
  input  wire  [LINE_WORDS*XLEN-1:0] mem_rline,
  output logic                    mem_we,
  output logic [31:0]             mem_waddr,
  output logic [LINE_WORDS*XLEN-1:0] mem_wline,
  // performance counters
  output logic [31:0]             perf_hits,
  output logic [31:0]             perf_misses
);

  localparam int OFFW = $clog2(LINE_WORDS);
  localparam int IDXW = $clog2(SETS);
  localparam int TAGW = 32 - IDXW - OFFW - 2;
  localparam int LINEBITS = LINE_WORDS * XLEN;

  logic [TAGW-1:0]     tag  [SETS];
  logic                vld  [SETS];
  logic                drty [SETS];
  logic [LINEBITS-1:0] data [SETS];

  // latched request
  logic [31:0] q_addr;
  logic        q_we;
  word_t       q_wdata;
  wire [OFFW-1:0] q_off = q_addr[2+OFFW-1 : 2];
  wire [IDXW-1:0] q_idx = q_addr[2+OFFW+IDXW-1 : 2+OFFW];
  wire [TAGW-1:0] q_tag = q_addr[31 : 2+OFFW+IDXW];
  wire hit = vld[q_idx] && (tag[q_idx] == q_tag);

  assign mem_raddr = {q_tag, q_idx, {(OFFW+2){1'b0}}};          // requested line
  assign mem_waddr = {tag[q_idx], q_idx, {(OFFW+2){1'b0}}};     // evicted (old) line
  assign mem_wline = data[q_idx];

  typedef enum logic [2:0] {S_IDLE, S_LOOKUP, S_WB, S_FILL, S_DONE} state_e;
  state_e state;

  always_ff @(posedge clk) begin
    if (rst) begin
      state <= S_IDLE; resp_ready <= 1'b0; mem_we <= 1'b0;
      perf_hits <= '0; perf_misses <= '0;
      for (int i = 0; i < SETS; i++) begin vld[i] <= 1'b0; drty[i] <= 1'b0; end
    end else begin
      resp_ready <= 1'b0; mem_we <= 1'b0;
      unique case (state)
        S_IDLE: if (req_valid) begin
          q_addr <= req_addr; q_we <= req_we; q_wdata <= req_wdata; state <= S_LOOKUP;
        end
        S_LOOKUP: begin
          if (hit) begin
            perf_hits <= perf_hits + 1'b1;
            resp_rdata <= data[q_idx][q_off*XLEN +: XLEN];
            if (q_we) begin data[q_idx][q_off*XLEN +: XLEN] <= q_wdata; drty[q_idx] <= 1'b1; end
            resp_hit <= 1'b1; resp_ready <= 1'b1; state <= S_IDLE;
          end else begin
            perf_misses <= perf_misses + 1'b1; resp_hit <= 1'b0;
            if (vld[q_idx] && drty[q_idx]) begin mem_we <= 1'b1; state <= S_WB; end
            else state <= S_FILL;
          end
        end
        S_WB:   state <= S_FILL;                       // writeback pulse issued during this cycle
        S_FILL: begin
          data[q_idx] <= mem_rline;                    // install fetched line
          tag[q_idx]  <= q_tag; vld[q_idx] <= 1'b1; drty[q_idx] <= 1'b0;
          state <= S_DONE;
        end
        S_DONE: begin
          resp_rdata <= data[q_idx][q_off*XLEN +: XLEN];
          if (q_we) begin data[q_idx][q_off*XLEN +: XLEN] <= q_wdata; drty[q_idx] <= 1'b1; end
          resp_ready <= 1'b1; state <= S_IDLE;
        end
      endcase
    end
  end

`ifndef SYNTHESIS
  a_resp_pulse: assert property (@(posedge clk) disable iff (rst) resp_ready |=> !resp_ready);
`endif

endmodule

`default_nettype wire
