// l1_dcache.sv — per-CU L1 data cache: line-granular, WRITE-THROUGH (M14, reworked M17).
//
// The L1 in front of each Compute Unit's coalesced LSU. M14 built it write-back; M17's
// multi-CU scale-out made it **write-through / no-write-allocate**, the policy real GPUs
// use for global memory, because private write-back L1s without coherence lose data to
// false sharing (two CUs evicting different words of the same line). Consequences:
//   * Loads allocate lines and hit locally (read caching is the L1's whole job).
//   * Stores update a present line (no dirty bit) and ALWAYS forward the strobed words
//     to the next level (the shared L2 merges word-strobes from all CUs).
//   * Atomics are forwarded to the next level (the L2 is the coherence point) and
//     SELF-INVALIDATE the local copy of the line.
//   * Flush = invalidate-all (nothing is ever dirty here), used at kernel boundaries.
// Cross-CU visibility is guaranteed at kernel boundaries and through atomics — the same
// contract CUDA gives for non-volatile global memory.
//
// LSU side: pulse req_valid one cycle (latched); resp_ready pulses on completion.
// Memory side (to the L2): LEVEL-based handshake — hold mem_req + fields until mem_ack
// pulses (the L2 arbitrates several L1s round-robin, so latency varies).
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module l1_dcache
  import sirion_pkg::*;
#(
  parameter int LINE_WORDS = 8,
  parameter int SETS       = 64
) (
  input  wire                     clk,
  input  wire                     rst,
  // LSU side (one in-flight transaction; req_valid is a 1-cycle pulse)
  input  wire                     req_valid,
  input  wire                     req_we,
  input  wire  [31:0]             req_addr,    // any byte address within the line
  input  wire  [LINE_WORDS*XLEN-1:0] req_wline,
  input  wire  [LINE_WORDS-1:0]   req_wstrb,   // per-word write enables
  input  wire                     req_atom,    // atomic word RMW (forwarded to L2)
  input  wire  [2:0]              req_afunc,
  input  wire  word_t             req_aword,
  input  wire  word_t             req_acmp,
  output word_t                   resp_old,    // atomic: pre-RMW word (with resp_ready)
  output logic                    resp_ready,
  output logic [LINE_WORDS*XLEN-1:0] resp_rline,
  output logic                    resp_hit,
  // flush (invalidate all lines; write-through means nothing to write back)
  input  wire                     flush,
  output logic                    flush_done,
  // memory side (level handshake to the shared L2)
  output logic                    mem_req,
  output logic                    mem_we,
  output logic                    mem_atom,
  output logic [2:0]              mem_afunc,
  output logic [31:0]             mem_addr,
  output logic [LINE_WORDS*XLEN-1:0] mem_wline,
  output logic [LINE_WORDS-1:0]   mem_wstrb,
  output word_t                   mem_aword,
  output word_t                   mem_acmp,
  input  wire                     mem_ack,     // 1-cycle pulse; rline/old valid with it
  input  wire  [LINE_WORDS*XLEN-1:0] mem_rline,
  input  wire  word_t             mem_old,
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
  logic [LINEBITS-1:0] data [SETS];

  // latched request
  logic [31:0]           q_addr;
  logic                  q_we, q_atom;
  logic [2:0]            q_afunc;
  word_t                 q_aword, q_acmp;
  logic [LINEBITS-1:0]   q_wline;
  logic [LINE_WORDS-1:0] q_wstrb;
  wire [IDXW-1:0] q_idx = q_addr[2+OFFW+IDXW-1 : 2+OFFW];
  wire [TAGW-1:0] q_tag = q_addr[31 : 2+OFFW+IDXW];
  wire hit = vld[q_idx] && (tag[q_idx] == q_tag);

  // flush walker
  logic [IDXW:0] f_set;
  wire  [IDXW-1:0] f_idx = f_set[IDXW-1:0];

  typedef enum logic [2:0] {S_IDLE, S_LOOKUP, S_FILL, S_STORE, S_ATOM, S_DONE, S_FLUSH} state_e;
  state_e state;

  // merge helper: overwrite strobed words of a line
  function automatic logic [LINEBITS-1:0] f_merge(
      input logic [LINEBITS-1:0] line,
      input logic [LINEBITS-1:0] wline,
      input logic [LINE_WORDS-1:0] strb);
    f_merge = line;
    for (int w = 0; w < LINE_WORDS; w++)
      if (strb[w]) f_merge[w*XLEN +: XLEN] = wline[w*XLEN +: XLEN];
  endfunction

  // memory-side request fields (held stable while mem_req is high).
  // Atomics carry the FULL word address (the L2 RMWs that word); line ops are aligned.
  assign mem_addr  = q_atom ? {q_addr[31:2], 2'b00}
                            : {q_addr[31:2+OFFW], {(OFFW+2){1'b0}}};
  assign mem_wline = q_wline;
  assign mem_wstrb = q_wstrb;
  assign mem_we    = q_we && !q_atom;
  assign mem_atom  = q_atom;
  assign mem_afunc = q_afunc;
  assign mem_aword = q_aword;
  assign mem_acmp  = q_acmp;
  // req deasserts COMBINATIONALLY on ack: the L2 samples req in the same cycle its ack
  // pulse is visible, so a still-high req would be picked (and executed) a second time —
  // harmless for idempotent loads/stores, catastrophic for atomics.
  assign mem_req   = ((state == S_FILL) || (state == S_STORE) || (state == S_ATOM)) && !mem_ack;

  always_ff @(posedge clk) begin
    if (rst) begin
      state <= S_IDLE; resp_ready <= 1'b0; flush_done <= 1'b0;
      resp_hit <= 1'b0; resp_rline <= '0; resp_old <= '0; f_set <= '0;
      q_addr <= '0; q_we <= 1'b0; q_wline <= '0; q_wstrb <= '0;
      q_atom <= 1'b0; q_afunc <= '0; q_aword <= '0; q_acmp <= '0;
      perf_hits <= '0; perf_misses <= '0;
      for (int i = 0; i < SETS; i++) vld[i] <= 1'b0;
    end else begin
      resp_ready <= 1'b0; flush_done <= 1'b0;
      unique case (state)
        S_IDLE: begin
          if (flush) begin
            f_set <= '0; state <= S_FLUSH;
          end else if (req_valid) begin
            q_addr <= req_addr; q_we <= req_we;
            q_wline <= req_wline; q_wstrb <= req_wstrb;
            q_atom <= req_atom; q_afunc <= req_afunc;
            q_aword <= req_aword; q_acmp <= req_acmp;
            state <= S_LOOKUP;
          end
        end

        S_LOOKUP: begin
          if (q_atom) begin
            if (hit) vld[q_idx] <= 1'b0;             // self-invalidate; L2 does the RMW
            state <= S_ATOM;
          end else if (q_we) begin
            if (hit) data[q_idx] <= f_merge(data[q_idx], q_wline, q_wstrb);
            state <= S_STORE;                        // write-through: always forward
          end else if (hit) begin
            perf_hits <= perf_hits + 1'b1;
            resp_rline <= data[q_idx];
            resp_hit <= 1'b1; resp_ready <= 1'b1; state <= S_IDLE;
          end else begin
            perf_misses <= perf_misses + 1'b1; resp_hit <= 1'b0;
            state <= S_FILL;
          end
        end

        S_FILL: if (mem_ack) begin                   // read-allocate
          data[q_idx] <= mem_rline;
          tag[q_idx]  <= q_tag; vld[q_idx] <= 1'b1;
          resp_rline  <= mem_rline;
          resp_ready  <= 1'b1; state <= S_IDLE;
        end

        S_STORE: if (mem_ack) begin                  // store acknowledged by the L2
          resp_ready <= 1'b1; state <= S_IDLE;
        end

        S_ATOM: if (mem_ack) begin                   // atomic RMW performed at the L2
          resp_old   <= mem_old;
          resp_ready <= 1'b1; state <= S_IDLE;
        end

        S_FLUSH: begin                               // invalidate-all (never dirty)
          if (f_set == SETS[IDXW:0]) begin
            flush_done <= 1'b1; state <= S_IDLE;
          end else begin
            vld[f_idx] <= 1'b0;
            f_set <= f_set + 1'b1;
          end
        end

        default: state <= S_IDLE;
      endcase
    end
  end

`ifndef SYNTHESIS
  a_resp_pulse:  assert property (@(posedge clk) disable iff (rst) resp_ready |=> !resp_ready);
  a_flush_pulse: assert property (@(posedge clk) disable iff (rst) flush_done |=> !flush_done);
`endif

endmodule

`default_nettype wire
