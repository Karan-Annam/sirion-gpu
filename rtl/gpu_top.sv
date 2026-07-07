// gpu_top.sv — Sirion GPU top level (M12, multi-CU M17).
//
// The host-facing device: NUM_CU compute units, each with a private write-through L1,
// sharing one write-back L2 (the coherence point) in front of the global memory.
// The dispatcher hands the blocks of a launch grid to whichever CU is free, so blocks
// genuinely execute in parallel. At grid end every L1 is invalidated and the L2 is
// flushed to memory, so `grid_done` guarantees host-coherent global memory.
//
//   host ──► launch descriptor ─► CTA DISPATCH ──► CU0 ── L1 ──┐
//        ──► imem/cbank (broadcast)          ├──► CU1 ── L1 ──┤── L2 ──► GLOBAL MEM
//        ◄── gdbg readback (post-flush)      └──► ...         ┘ (RR arbiter, atomics)
//
// Debug readback: dbg_cu selects which CU's register file / predicates to read.
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module gpu_top
  import sirion_pkg::*;
#(
  parameter  int NUM_CU      = 1,
  parameter  int IMEM_WORDS  = 1024,
  parameter  int CBANK_WORDS = 64,
  parameter  int GMEM_WORDS  = 8192,   // 32 KB word-addressed global memory
  parameter  int SMEM_WORDS  = 4096,
  parameter  int NUM_WARPS   = 8,
  parameter  int LINE_WORDS  = 8,
  parameter  int L2_SETS     = 256,
  localparam int WW = (NUM_WARPS > 1) ? $clog2(NUM_WARPS) : 1,
  localparam int CW = (NUM_CU > 1) ? $clog2(NUM_CU) : 1
) (
  input  wire                          clk,
  input  wire                          rst,
  // ---- host: program / constant / memory load + readback ----
  input  wire                          imem_we,
  input  wire [$clog2(IMEM_WORDS)-1:0] imem_waddr,
  input  wire  insn_t                  imem_wdata,
  input  wire                          cbank_we,
  input  wire [$clog2(CBANK_WORDS)-1:0] cbank_waddr,
  input  wire  word_t                  cbank_wdata,
  input  wire                          gmem_we,
  input  wire [$clog2(GMEM_WORDS)-1:0] gmem_waddr,
  input  wire  word_t                  gmem_wdata,
  input  wire [$clog2(GMEM_WORDS)-1:0] gdbg_addr,
  output word_t                        gdbg_data,
  // ---- host: kernel launch descriptor ----
  input  wire                          launch,     // 1-cycle pulse; descriptor held stable
  input  wire  word_t                  grid_nx,    // grid size in blocks (1D)
  input  wire  word_t                  tpb,        // threads per block
  input  wire [$clog2(IMEM_WORDS)-1:0] entry,      // kernel entry PC
  // ---- status ----
  output logic                         grid_busy,
  output logic                         grid_done,  // 1-cycle pulse (memory is coherent)
  output logic [31:0]                  perf_blocks,
  output logic [31:0]                  perf_cycles,  // CU0 cycles with a block resident
  output logic [31:0]                  perf_insns,   // instructions retired, ALL CUs
  output logic [31:0]                  perf_memops,  // memory instructions, ALL CUs
  output logic [31:0]                  perf_l1_hits, // L1 hits, ALL CUs
  output logic [31:0]                  perf_l1_misses,
  output logic [31:0]                  perf_l2_hits,
  output logic [31:0]                  perf_l2_misses,
  // ---- debug register/predicate readout (valid when idle) ----
  input  wire  [CW-1:0]                dbg_cu,
  input  wire  [WW-1:0]                dbg_warp,
  input  wire  vreg_id_t               dbg_addr,
  output warp_vec_t                    dbg_data,
  output warp_mask_t                   dbg_pred1,
  output warp_mask_t                   dbg_pred2,
  output warp_mask_t                   dbg_pred3
);

  localparam int GMW = $clog2(GMEM_WORDS);

  // ---- global memory (the "memory controller" model: 1-cycle line access) ----
  word_t gmem [GMEM_WORDS];
  logic [31:0]                l2m_raddr, l2m_waddr;
  logic [LINE_WORDS*XLEN-1:0] l2m_rline, l2m_wline;
  logic                       l2m_we;

  generate
    for (genvar lw = 0; lw < LINE_WORDS; lw++) begin : g_gmem_rd
      assign l2m_rline[lw*XLEN +: XLEN] = gmem[l2m_raddr[GMW+1:2] + GMW'(lw)];
    end
  endgenerate
  always_ff @(posedge clk) begin
    if (gmem_we)
      gmem[gmem_waddr] <= gmem_wdata;              // host preload (device idle)
    else if (l2m_we)
      for (int w = 0; w < LINE_WORDS; w++)
        gmem[l2m_waddr[GMW+1:2] + GMW'(w)] <= l2m_wline[w*XLEN +: XLEN];
  end
  assign gdbg_data = gmem[gdbg_addr];

  // ---- per-CU wiring ----
  logic [NUM_CU-1:0] cu_start, cu_busy, cu_done, cu_flush, cu_flush_done;
  word_t             cu_bidx [NUM_CU];
  word_t             disp_tpb;
  logic [$clog2(IMEM_WORDS)-1:0] disp_entry;

  logic [NUM_CU-1:0]          c_req, c_we, c_atom, c_ack;
  logic [2:0]                 c_afunc [NUM_CU];
  logic [31:0]                c_addr  [NUM_CU];
  logic [LINE_WORDS*XLEN-1:0] c_wline [NUM_CU];
  logic [LINE_WORDS-1:0]      c_wstrb [NUM_CU];
  word_t                      c_aword [NUM_CU];
  word_t                      c_acmp  [NUM_CU];
  logic [LINE_WORDS*XLEN-1:0] c_rline;
  word_t                      c_old;

  logic [31:0] p_cycles [NUM_CU];
  logic [31:0] p_insns  [NUM_CU], p_memops [NUM_CU];
  logic [31:0] p_l1h    [NUM_CU], p_l1m    [NUM_CU];
  warp_vec_t   dbgd     [NUM_CU];
  warp_mask_t  dbgp1    [NUM_CU], dbgp2 [NUM_CU], dbgp3 [NUM_CU];

  generate
    for (genvar c = 0; c < NUM_CU; c++) begin : g_cu
      cu_core #(
        .IMEM_WORDS(IMEM_WORDS), .CBANK_WORDS(CBANK_WORDS),
        .SMEM_WORDS(SMEM_WORDS), .NUM_WARPS(NUM_WARPS), .LINE_WORDS(LINE_WORDS)
      ) u_cu (
        .clk(clk), .rst(rst),
        .imem_we(imem_we), .imem_waddr(imem_waddr), .imem_wdata(imem_wdata),
        .cbank_we(cbank_we), .cbank_waddr(cbank_waddr), .cbank_wdata(cbank_wdata),
        .mem_req(c_req[c]), .mem_we(c_we[c]), .mem_atom(c_atom[c]),
        .mem_afunc(c_afunc[c]), .mem_addr(c_addr[c]), .mem_wline(c_wline[c]),
        .mem_wstrb(c_wstrb[c]), .mem_aword(c_aword[c]), .mem_acmp(c_acmp[c]),
        .mem_ack(c_ack[c]), .mem_rline(c_rline), .mem_old(c_old),
        .start(cu_start[c]), .entry(disp_entry),
        .ctx_bdx(disp_tpb), .ctx_gdx(grid_nx), .ctx_bidx(cu_bidx[c]), .ctx_tpb(disp_tpb),
        .busy(cu_busy[c]), .done(cu_done[c]),
        .flush(cu_flush[c]), .flush_done(cu_flush_done[c]),
        .perf_cycles(p_cycles[c]), .perf_insns(p_insns[c]), .perf_memops(p_memops[c]),
        .perf_l1_hits(p_l1h[c]), .perf_l1_misses(p_l1m[c]),
        .dbg_warp(dbg_warp), .dbg_addr(dbg_addr), .dbg_data(dbgd[c]),
        .dbg_pred1(dbgp1[c]), .dbg_pred2(dbgp2[c]), .dbg_pred3(dbgp3[c])
      );
    end
  endgenerate

  assign dbg_data  = dbgd[dbg_cu];
  assign dbg_pred1 = dbgp1[dbg_cu];
  assign dbg_pred2 = dbgp2[dbg_cu];
  assign dbg_pred3 = dbgp3[dbg_cu];
  assign perf_cycles = p_cycles[0];

  always_comb begin
    perf_insns = '0; perf_memops = '0; perf_l1_hits = '0; perf_l1_misses = '0;
    for (int c = 0; c < NUM_CU; c++) begin
      perf_insns     = perf_insns     + p_insns[c];
      perf_memops    = perf_memops    + p_memops[c];
      perf_l1_hits   = perf_l1_hits   + p_l1h[c];
      perf_l1_misses = perf_l1_misses + p_l1m[c];
    end
  end

  // ---- shared L2 ----
  logic l2_flush, l2_flush_done;
  l2_cache #(.NUM_PORTS(NUM_CU), .LINE_WORDS(LINE_WORDS), .SETS(L2_SETS)) u_l2 (
    .clk(clk), .rst(rst),
    .p_req(c_req), .p_we(c_we), .p_atom(c_atom), .p_afunc(c_afunc),
    .p_addr(c_addr), .p_wline(c_wline), .p_wstrb(c_wstrb),
    .p_aword(c_aword), .p_acmp(c_acmp),
    .p_ack(c_ack), .p_rline(c_rline), .p_old(c_old),
    .flush(l2_flush), .flush_done(l2_flush_done),
    .mem_raddr(l2m_raddr), .mem_rline(l2m_rline),
    .mem_we(l2m_we), .mem_waddr(l2m_waddr), .mem_wline(l2m_wline),
    .perf_hits(perf_l2_hits), .perf_misses(perf_l2_misses)
  );

  // ---- multi-CU workgroup dispatcher ----
  cta_dispatch #(.IMEM_WORDS(IMEM_WORDS), .NUM_CU(NUM_CU)) u_dispatch (
    .clk(clk), .rst(rst),
    .launch(launch), .grid_nx(grid_nx), .tpb(tpb), .entry(entry),
    .cu_start(cu_start), .cu_bidx(cu_bidx), .cu_tpb(disp_tpb), .cu_entry(disp_entry),
    .cu_busy(cu_busy), .cu_done(cu_done),
    .cu_flush(cu_flush), .cu_flush_done(cu_flush_done),
    .l2_flush(l2_flush), .l2_flush_done(l2_flush_done),
    .grid_busy(grid_busy), .grid_done(grid_done), .perf_blocks(perf_blocks)
  );

endmodule

`default_nettype wire
