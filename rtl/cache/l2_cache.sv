// l2_cache.sv — shared L2 cache + port arbiter (M17).
//
// The point of coherence for global memory across the CU array. Each CU's write-through
// L1 presents a level-based request (hold req until ack): line reads (L1 fills), strobed
// line writes (write-through stores), and atomic word RMWs. A round-robin arbiter picks
// one pending port at a time; the cache is direct-mapped, WRITE-BACK, write-allocate
// (it is the only writer of the backing global memory).
//
//  * Strobed writes merge per-word — this is what makes word-disjoint stores from
//    different CUs to the SAME line safe (no false sharing).
//  * Atomics RMW the addressed word in the L2 line and return the old value — one
//    serialization point for the whole GPU.
//  * Flush (host/dispatcher, at kernel boundaries): write back all dirty lines and
//    invalidate, so the backing memory is coherent for host readback.
//
// Backing memory: line-granular, combinational read + synchronous write (the memory
// controller / global memory model in gpu_top).
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module l2_cache
  import sirion_pkg::*;
#(
  parameter int NUM_PORTS  = 4,
  parameter int LINE_WORDS = 8,
  parameter int SETS       = 256,
  localparam int PW = (NUM_PORTS > 1) ? $clog2(NUM_PORTS) : 1
) (
  input  wire                     clk,
  input  wire                     rst,
  // CU-side ports (level handshake per port; fields held stable while req is high)
  input  wire  [NUM_PORTS-1:0]    p_req,
  input  wire  [NUM_PORTS-1:0]    p_we,
  input  wire  [NUM_PORTS-1:0]    p_atom,
  input  wire  [2:0]              p_afunc [NUM_PORTS],
  input  wire  [31:0]             p_addr  [NUM_PORTS],
  input  wire  [LINE_WORDS*XLEN-1:0] p_wline [NUM_PORTS],
  input  wire  [LINE_WORDS-1:0]   p_wstrb [NUM_PORTS],
  input  wire  word_t             p_aword [NUM_PORTS],
  input  wire  word_t             p_acmp  [NUM_PORTS],
  output logic [NUM_PORTS-1:0]    p_ack,        // 1-cycle pulse per port
  output logic [LINE_WORDS*XLEN-1:0] p_rline,   // shared response buses (valid with ack)
  output word_t                   p_old,
  // flush
  input  wire                     flush,
  output logic                    flush_done,
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

  // ---- round-robin port arbiter (explicit modulo: NUM_PORTS need not be a power of 2) ----
  logic [PW-1:0] rr_ptr, cur_port;
  logic          have_pick;
  logic [PW-1:0] pick;
  always_comb begin
    have_pick = 1'b0; pick = '0;
    for (int i = 1; i <= NUM_PORTS; i++) begin
      int p;
      p = (int'(rr_ptr) + i) % NUM_PORTS;
      if (!have_pick && p_req[p]) begin pick = PW'(p); have_pick = 1'b1; end
    end
  end

  // latched request (from the picked port)
  logic [31:0]           q_addr;
  logic                  q_we, q_atom;
  logic [2:0]            q_afunc;
  word_t                 q_aword, q_acmp;
  logic [LINEBITS-1:0]   q_wline;
  logic [LINE_WORDS-1:0] q_wstrb;
  wire [OFFW-1:0] q_off = q_addr[2+OFFW-1 : 2];
  wire [IDXW-1:0] q_idx = q_addr[2+OFFW+IDXW-1 : 2+OFFW];
  wire [TAGW-1:0] q_tag = q_addr[31 : 2+OFFW+IDXW];
  wire hit = vld[q_idx] && (tag[q_idx] == q_tag);

  // flush walker
  logic [IDXW:0] f_set;
  wire  [IDXW-1:0] f_idx = f_set[IDXW-1:0];

  typedef enum logic [2:0] {S_IDLE, S_LOOKUP, S_WB, S_FILL, S_SERVE, S_FLUSH, S_FLUSH_WB} state_e;
  state_e state;

  wire flushing = (state == S_FLUSH) || (state == S_FLUSH_WB);
  assign mem_raddr = {q_tag, q_idx, {(OFFW+2){1'b0}}};
  assign mem_waddr = flushing ? {tag[f_idx], f_idx, {(OFFW+2){1'b0}}}
                              : {tag[q_idx], q_idx, {(OFFW+2){1'b0}}};
  assign mem_wline = flushing ? data[f_idx] : data[q_idx];

  function automatic logic [LINEBITS-1:0] f_merge(
      input logic [LINEBITS-1:0] line,
      input logic [LINEBITS-1:0] wline,
      input logic [LINE_WORDS-1:0] strb);
    f_merge = line;
    for (int w = 0; w < LINE_WORDS; w++)
      if (strb[w]) f_merge[w*XLEN +: XLEN] = wline[w*XLEN +: XLEN];
  endfunction

  function automatic word_t f_atom(input logic [2:0] fn, input word_t old,
                                   input word_t val, input word_t cmp);
    unique case (fn)
      ATOM_ADD:  f_atom = old + val;
      ATOM_MIN:  f_atom = ($signed(val) < $signed(old)) ? val : old;
      ATOM_MAX:  f_atom = ($signed(val) > $signed(old)) ? val : old;
      ATOM_EXCH: f_atom = val;
      ATOM_CAS:  f_atom = (old == cmp) ? val : old;
      default:   f_atom = old;
    endcase
  endfunction

  // serve the latched request on the (now-present) line
  task automatic t_serve();
    p_rline <= data[q_idx];
    if (q_atom) begin
      p_old <= data[q_idx][q_off*XLEN +: XLEN];
      data[q_idx][q_off*XLEN +: XLEN] <=
        f_atom(q_afunc, data[q_idx][q_off*XLEN +: XLEN], q_aword, q_acmp);
      drty[q_idx] <= 1'b1;
    end else if (q_we) begin
      data[q_idx] <= f_merge(data[q_idx], q_wline, q_wstrb);
      drty[q_idx] <= 1'b1;
    end
    p_ack[cur_port] <= 1'b1;
  endtask

  always_ff @(posedge clk) begin
    if (rst) begin
      state <= S_IDLE; p_ack <= '0; flush_done <= 1'b0; mem_we <= 1'b0;
      rr_ptr <= '0; cur_port <= '0; f_set <= '0;
      q_addr <= '0; q_we <= 1'b0; q_atom <= 1'b0; q_afunc <= '0;
      q_aword <= '0; q_acmp <= '0; q_wline <= '0; q_wstrb <= '0;
      p_rline <= '0; p_old <= '0;
      perf_hits <= '0; perf_misses <= '0;
      for (int i = 0; i < SETS; i++) begin vld[i] <= 1'b0; drty[i] <= 1'b0; end
    end else begin
      p_ack <= '0; flush_done <= 1'b0; mem_we <= 1'b0;
      unique case (state)
        S_IDLE: begin
          if (flush) begin
            f_set <= '0; state <= S_FLUSH;
          end else if (have_pick) begin
            cur_port <= pick; rr_ptr <= pick;
            q_addr  <= p_addr[pick];  q_we    <= p_we[pick];
            q_atom  <= p_atom[pick];  q_afunc <= p_afunc[pick];
            q_aword <= p_aword[pick]; q_acmp  <= p_acmp[pick];
            q_wline <= p_wline[pick]; q_wstrb <= p_wstrb[pick];
            state <= S_LOOKUP;
          end
        end

        S_LOOKUP: begin
          if (hit) begin
            perf_hits <= perf_hits + 1'b1;
            t_serve();
            state <= S_IDLE;
          end else begin
            perf_misses <= perf_misses + 1'b1;
            if (vld[q_idx] && drty[q_idx]) begin mem_we <= 1'b1; state <= S_WB; end
            else state <= S_FILL;
          end
        end

        S_WB:   state <= S_FILL;                 // writeback pulse fires during this cycle
        S_FILL: begin
          data[q_idx] <= mem_rline;
          tag[q_idx]  <= q_tag; vld[q_idx] <= 1'b1; drty[q_idx] <= 1'b0;
          state <= S_SERVE;
        end
        S_SERVE: begin
          t_serve();
          state <= S_IDLE;
        end

        S_FLUSH: begin
          if (f_set == SETS[IDXW:0]) begin
            flush_done <= 1'b1; state <= S_IDLE;
          end else if (vld[f_idx] && drty[f_idx]) begin
            mem_we <= 1'b1;                      // pulse lands next cycle: f_set holds
            state  <= S_FLUSH_WB;
          end else begin
            vld[f_idx] <= 1'b0;
            f_set <= f_set + 1'b1;
          end
        end
        S_FLUSH_WB: begin                        // mem_we active THIS cycle, f_idx stable
          vld[f_idx]  <= 1'b0;
          drty[f_idx] <= 1'b0;
          f_set <= f_set + 1'b1;
          state <= S_FLUSH;
        end
      endcase
    end
  end

`ifndef SYNTHESIS
  a_flush_pulse: assert property (@(posedge clk) disable iff (rst) flush_done |=> !flush_done);
  a_one_ack:     assert property (@(posedge clk) disable iff (rst) $onehot0(p_ack));
`endif

endmodule

`default_nettype wire
