// cu_core.sv — Sirion multi-warp SIMT compute unit with a barrel pipeline (M13).
//
// M3–M6 datapath/SIMT/memory + M11 multi-warp/barrier, re-microarchitected in M13 from a
// multicycle FSM (~5 cycles/instruction, one warp at a time) into a 4-stage
// **barrel pipeline** — the classic GPU fine-grained-multithreading design:
//
//   PICK ──► FETCH ──► READ ──► EXEC ──► (MEM unit, parallel)
//   pick a ready  imem read   regfile    ALU/branch/writeback; memory ops are handed to a
//   warp; pop     (1 cycle)   read       lane-walking memory unit that runs WHILE the
//   dead frames               (1 cycle)  pipeline keeps executing other warps
//
// Each stage holds a DIFFERENT warp and at most ONE instruction per warp is in flight
// (in_flight[w]), so there are no intra-warp RAW/WAW hazards by construction — the per-warp
// in-flight bit *is* the scoreboard. With >=4 ready warps the core issues one instruction
// per cycle (~1 IPC aggregate, ~5x the multicycle core); with one warp it degrades
// gracefully to one instruction per 4 cycles. A load's latency is hidden by issuing other
// warps — the reason GPUs are latency-tolerant.
//
// The memory unit walks only the ACTIVE lanes of a request (priority-encoded), so divergent
// memory ops cost #active-lanes cycles, not WARP_SIZE. It has its own register-file write
// port, so a load writeback never stalls an ALU writeback (they are always different warps).
// Structural hazard: a second memory op reaching EXEC while the unit is busy stalls the
// pipeline (stall holds PICK/FETCH/READ/EXEC and the registered RF read data).
//
// Everything architectural is unchanged from M11: reconvergence stacks, predication, BAR
// barrier semantics (park + release when all live warps arrive), special registers, LDC/
// LDG/STG/LDS/STS, per-warp present derived from ctx_tpb. RTL == golden ISS bit-exact.
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module cu_core
  import sirion_pkg::*;
#(
  parameter  int IMEM_WORDS  = 1024,
  parameter  int STACK_DEPTH = 64,    // deep enough for a fully-divergent 32-lane loop
  parameter  int CBANK_WORDS = 64,
  parameter  int SMEM_WORDS  = 4096,  // 16 KB per-workgroup shared memory
  parameter  int NUM_WARPS   = 8,     // resident warps per block (power of two)
  parameter  int LINE_WORDS  = 8,     // L1 line size (words) — coalescing granule
  parameter  int L1_SETS     = 64,
  localparam int WW = (NUM_WARPS > 1) ? $clog2(NUM_WARPS) : 1
) (
  input  wire                          clk,
  input  wire                          rst,
  // program load
  input  wire                          imem_we,
  input  wire [$clog2(IMEM_WORDS)-1:0] imem_waddr,
  input  wire  insn_t                  imem_wdata,
  // constant-bank load (kernel parameters)
  input  wire                          cbank_we,
  input  wire [$clog2(CBANK_WORDS)-1:0] cbank_waddr,
  input  wire  word_t                  cbank_wdata,
  // global-memory port (M17): this CU's L1 talking to the shared L2 (level handshake)
  output logic                         mem_req,
  output logic                         mem_we,
  output logic                         mem_atom,
  output logic [2:0]                   mem_afunc,
  output logic [31:0]                  mem_addr,
  output logic [LINE_WORDS*XLEN-1:0]   mem_wline,
  output logic [LINE_WORDS-1:0]        mem_wstrb,
  output word_t                        mem_aword,
  output word_t                        mem_acmp,
  input  wire                          mem_ack,
  input  wire  [LINE_WORDS*XLEN-1:0]   mem_rline,
  input  wire  word_t                  mem_old,
  // launch (one whole block of ceil(tpb/32) resident warps)
  input  wire                          start,
  input  wire [$clog2(IMEM_WORDS)-1:0] entry,
  // special-register context (1D subset)
  input  wire  word_t                  ctx_bdx,
  input  wire  word_t                  ctx_gdx,
  input  wire  word_t                  ctx_bidx,
  input  wire  word_t                  ctx_tpb,
  // status
  output logic                         busy,
  output logic                         done,
  // cache maintenance: flush (write back + invalidate) before host gmem readback
  input  wire                          flush,
  output logic                         flush_done,
  // performance counters (free-running across blocks; cleared at reset)
  output logic [31:0]                  perf_cycles,   // cycles with a block resident
  output logic [31:0]                  perf_insns,    // instructions retired (EXEC)
  output logic [31:0]                  perf_memops,   // memory instructions dispatched
  output logic [31:0]                  perf_l1_hits,  // L1 line-transaction hits
  output logic [31:0]                  perf_l1_misses,
  // debug register/predicate readout (select a warp with dbg_warp; valid when done)
  input  wire  [WW-1:0]                dbg_warp,
  input  wire  vreg_id_t               dbg_addr,
  output warp_vec_t                    dbg_data,
  output warp_mask_t                   dbg_pred1,
  output warp_mask_t                   dbg_pred2,
  output warp_mask_t                   dbg_pred3
);

  localparam int AW  = $clog2(IMEM_WORDS);
  localparam int PCW = AW + 1;
  localparam int SPW = $clog2(STACK_DEPTH + 1);
  localparam int CBW = $clog2(CBANK_WORDS);
  localparam int SMW = $clog2(SMEM_WORDS);
  localparam logic [PCW-1:0] DONE = '1;

  typedef struct packed {
    warp_mask_t     mask;
    logic [PCW-1:0] pc;
    logic [PCW-1:0] rpc;
    logic [PCW-1:0] pend;
  } frame_t;

  // ---- memories (per-CU; global memory lives behind the L1 -> L2 port) ----
  insn_t imem  [IMEM_WORDS];
  word_t cbank [CBANK_WORDS];
  word_t smem  [SMEM_WORDS];

  always_ff @(posedge clk) if (imem_we)  imem[imem_waddr]   <= imem_wdata;
  always_ff @(posedge clk) if (cbank_we) cbank[cbank_waddr] <= cbank_wdata;

  // ---- per-warp architectural state ----
  frame_t             stack     [NUM_WARPS][STACK_DEPTH];
  logic [SPW-1:0]     sp        [NUM_WARPS];
  warp_mask_t         exited    [NUM_WARPS];
  warp_mask_t         pred1     [NUM_WARPS], pred2 [NUM_WARPS], pred3 [NUM_WARPS];
  logic [NUM_WARPS-1:0] finished, at_barrier, resident, in_flight;

  logic running;       // a block is resident
  logic block_done;

  assign busy = running;
  assign done = block_done;
  assign dbg_pred1 = pred1[dbg_warp];
  assign dbg_pred2 = pred2[dbg_warp];
  assign dbg_pred3 = pred3[dbg_warp];

  // ---- per-warp present mask & residency, derived from threads-per-block ----
  warp_mask_t present_w  [NUM_WARPS];
  logic       resident_w [NUM_WARPS];
  always_comb begin
    for (int w = 0; w < NUM_WARPS; w++) begin
      present_w[w] = '0;
      for (int l = 0; l < WARP_SIZE; l++)
        if (unsigned'(w*WARP_SIZE + l) < ctx_tpb) present_w[w][l] = 1'b1;
      resident_w[w] = (unsigned'(w*WARP_SIZE) < ctx_tpb);
    end
  end

  // ================================================================================
  // Memory unit (parallel to the pipeline). SHARED memory walks active lanes serially
  // against the scratchpad. GLOBAL memory goes through a COALESCER: active lanes are
  // grouped by cache line and each group becomes ONE line transaction on the L1 D$ —
  // a fully-coalesced 32-lane access is 32/LINE_WORDS transactions, not 32 walks.
  // Own RF write port for load results; other warps keep issuing meanwhile.
  // ================================================================================
  localparam int OFFW = $clog2(LINE_WORDS);

  logic        m_busy, m_ld, m_shared, m_atom;
  logic [2:0]  m_afunc;
  logic [WW-1:0] m_warp;
  vreg_id_t    m_rd;
  word_t       m_imm;
  warp_mask_t  m_gm, m_pending;
  warp_vec_t   m_base, m_sdata, m_ldvec, m_cmpv;
  logic        c_wait;                 // a line transaction is in flight on the L1
  warp_mask_t  c_match;                // lanes covered by the in-flight transaction

  function automatic logic [LANE_ID_W-1:0] f_lowbit(input warp_mask_t m);
    f_lowbit = '0;
    for (int i = WARP_SIZE-1; i >= 0; i--)
      if (m[i]) f_lowbit = i[LANE_ID_W-1:0];
  endfunction

  // per-lane byte address / line id / word-in-line for the latched request
  wire [XLEN-1:0] lane_byte [WARP_SIZE];
  generate
    for (genvar ml = 0; ml < WARP_SIZE; ml++) begin : g_laddr
      assign lane_byte[ml] = m_base[ml] + m_imm;
    end
  endgenerate

  // ---- shared-memory serial walk ----
  wire [LANE_ID_W-1:0] m_lane   = f_lowbit(m_pending);
  wire [SMW-1:0]       m_word_s = lane_byte[m_lane][SMW+1:2];
  wire m_step = m_busy && m_shared && (m_pending != '0);

  // ---- global-memory coalescer (atomics never coalesce: one lane per transaction) ----
  wire [XLEN-1:0] g_byte = lane_byte[m_lane];               // leader lane's address
  warp_mask_t  g_match_ln;                                  // pending lanes on the same line
  always_comb
    for (int i = 0; i < WARP_SIZE; i++)
      g_match_ln[i] = m_pending[i] &&
                      (lane_byte[i][31:2+OFFW] == g_byte[31:2+OFFW]);
  wire warp_mask_t g_match = m_atom ? (warp_mask_t'(1) << m_lane) : g_match_ln;

  // store line assembly: word w takes the HIGHEST matched lane targeting it (ISS lane order)
  logic [LINE_WORDS*XLEN-1:0] g_wline;
  logic [LINE_WORDS-1:0]      g_wstrb;
  always_comb begin
    g_wline = '0; g_wstrb = '0;
    for (int i = 0; i < WARP_SIZE; i++)
      if (g_match[i]) begin
        g_wstrb[lane_byte[i][2+OFFW-1:2]] = 1'b1;
        g_wline[lane_byte[i][2+OFFW-1:2]*XLEN +: XLEN] = m_sdata[i];
      end
  end

  wire g_req = m_busy && !m_shared && !c_wait && (m_pending != '0);
  wire m_complete = m_busy && (m_pending == '0) && !c_wait;   // all lanes done -> retire

  // ---- L1 data cache (line granular; memory side exported to the shared L2) ----
  logic                       l1_resp_ready;
  logic [LINE_WORDS*XLEN-1:0] l1_resp_rline;

  word_t l1_resp_old;
  l1_dcache #(.LINE_WORDS(LINE_WORDS), .SETS(L1_SETS)) u_l1 (
    .clk(clk), .rst(rst),
    .req_valid(g_req), .req_we(!m_ld && !m_atom), .req_addr(g_byte),
    .req_wline(g_wline), .req_wstrb(g_wstrb),
    .req_atom(m_atom), .req_afunc(m_afunc),
    .req_aword(m_sdata[m_lane]), .req_acmp(m_cmpv[m_lane]),
    .resp_old(l1_resp_old),
    .resp_ready(l1_resp_ready), .resp_rline(l1_resp_rline), .resp_hit(),
    .flush(flush), .flush_done(flush_done),
    .mem_req(mem_req), .mem_we(mem_we), .mem_atom(mem_atom), .mem_afunc(mem_afunc),
    .mem_addr(mem_addr), .mem_wline(mem_wline), .mem_wstrb(mem_wstrb),
    .mem_aword(mem_aword), .mem_acmp(mem_acmp),
    .mem_ack(mem_ack), .mem_rline(mem_rline), .mem_old(mem_old),
    .perf_hits(perf_l1_hits), .perf_misses(perf_l1_misses)
  );

  // ================================================================================
  // Pipeline
  // ================================================================================
  // PICK -> FETCH
  logic          pf_valid;
  logic [WW-1:0] pf_warp;
  logic [PCW-1:0] pf_pc;
  // FETCH -> READ
  logic          fr_valid;
  logic [WW-1:0] fr_warp;
  logic [PCW-1:0] fr_pc;
  insn_t         fr_ir;
  // READ -> EXEC
  logic          re_valid;
  logic [WW-1:0] re_warp;
  logic [PCW-1:0] re_pc;
  insn_t         re_ir;

  // ---- decode at READ (register addresses) and at EXEC (full control) ----
  vreg_id_t  r_rd, r_rs1, r_rs2;
  logic      r_is_st, r_is_setp;
  decode u_dec_r (
    .insn(fr_ir), .opcode(), .gneg(), .psel(),
    .rd(r_rd), .rs1(r_rs1), .rs2(r_rs2), .pd(), .cmp(), .imm(),
    .reg_write(), .is_alu(), .is_load(), .is_store(r_is_st),
    .mem_shared(), .is_branch(), .is_ssy(), .is_setp(r_is_setp),
    .is_rdsr(), .uses_rs1(), .uses_rs2(), .alu_a_imm(), .alu_b_imm()
  );

  logic [OPC_W-1:0] opcode;
  logic             gneg;
  logic [1:0]       psel, pd;
  logic [2:0]       cmp_code;
  vreg_id_t         rd, rs1, rs2;
  word_t            imm;
  logic             is_alu, is_branch, is_ssy, is_setp, is_rdsr;
  logic             alu_a_imm, alu_b_imm;
  decode u_dec_e (
    .insn(re_ir), .opcode(opcode), .gneg(gneg), .psel(psel),
    .rd(rd), .rs1(rs1), .rs2(rs2), .pd(pd), .cmp(cmp_code), .imm(imm),
    .reg_write(), .is_alu(is_alu), .is_load(), .is_store(),
    .mem_shared(), .is_branch(is_branch), .is_ssy(is_ssy), .is_setp(is_setp),
    .is_rdsr(is_rdsr), .uses_rs1(), .uses_rs2(), .alu_a_imm(alu_a_imm), .alu_b_imm(alu_b_imm)
  );
  wire is_exit = (opcode == OP_EXIT);
  wire is_sync = (opcode == OP_SYNC);
  wire is_bar  = (opcode == OP_BAR);
  wire is_setpi= (opcode == OP_ISETPI);
  wire is_fsetp= (opcode == OP_FSETP);
  wire is_falu = (opcode >= 6'h35) && (opcode <= 6'h3C);   // FADD..F2I (M15)
  wire is_mufu = (opcode == OP_MUFU);                      // SFU (M16)
  wire is_atomg= (opcode == OP_ATOMG);                     // atomics (M16)
  wire is_atoms= (opcode == OP_ATOMS);
  wire is_atom = is_atomg | is_atoms;
  wire is_ldc  = (opcode == OP_LDC);
  wire is_ldg  = (opcode == OP_LDG);
  wire is_stg  = (opcode == OP_STG);
  wire is_lds  = (opcode == OP_LDS);
  wire is_sts  = (opcode == OP_STS);
  wire is_ld_mem   = is_ldg | is_lds;
  wire is_st_mem   = is_stg | is_sts;
  wire is_shared   = is_lds | is_sts | is_atoms;
  wire is_mem_loop = is_ld_mem | is_st_mem | is_atom;

  // ---- structural stall: memory op at EXEC while the memory unit is busy ----
  // (m_complete frees the unit this cycle, so back-to-back handoff is allowed.)
  wire stall = re_valid && is_mem_loop && m_busy && !m_complete;

  // ---- warp scheduler: ready = resident, not finished/parked/in-flight ----
  logic [NUM_WARPS-1:0] sched_ready;
  logic                 sched_valid, sched_advance;
  logic [WW-1:0]        sched_sel;
  always_comb
    for (int w = 0; w < NUM_WARPS; w++)
      sched_ready[w] = resident[w] & ~finished[w] & ~at_barrier[w] & ~in_flight[w];

  warp_sched #(.NUM_WARPS(NUM_WARPS)) u_sched (
    .clk(clk), .rst(rst),
    .ready(sched_ready), .advance(sched_advance),
    .valid(sched_valid), .sel(sched_sel)
  );

  // ---- PICK-stage combinational decision on the selected warp ----
  wire [SPW-1:0] sp_p  = sp[sched_sel];
  frame_t        tos_p;
  assign tos_p = (sp_p == 0) ? '0 : stack[sched_sel][sp_p-1];
  wire pick_act   = running && sched_valid && !stall;
  wire pick_fin   = pick_act && (sp_p == 0);
  wire pick_pop   = pick_act && (sp_p != 0) &&
                    (((tos_p.mask & ~exited[sched_sel]) == '0) || (tos_p.pc == tos_p.rpc));
  wire pick_issue = pick_act && (sp_p != 0) && !pick_pop;
  assign sched_advance = pick_act;

  wire any_parked   = |(resident & ~finished & at_barrier);
  wire any_inflight = |in_flight;
  wire any_ready    = |sched_ready;

  // ---- register file (per-warp banked, 3 read + 2 write ports) ----
  logic       rf_ren0, rf_ren1;
  vreg_id_t   rf_raddr0, rf_raddr1;
  warp_vec_t  rf_rdata0, rf_rdata1, rf_rdata2;

  // port0 = base (mem) / rs1 (alu) / prs1 (setp) / dbg (done)
  // port1 = store data reg rd (stg/sts) / rs2 (alu) / prs2 (setp)
  // port2 = rd (FFMA accumulator, M15)
  wire vreg_id_t rd_src0 = r_is_setp ? insn_prs1(fr_ir) : r_rs1;
  wire vreg_id_t rd_src1 = r_is_st   ? r_rd : (r_is_setp ? insn_prs2(fr_ir) : r_rs2);

  wire dbg_read = done;                      // pipeline is empty whenever done is high
  assign rf_ren0   = (fr_valid && !stall) | dbg_read;
  assign rf_ren1   = (fr_valid && !stall);
  assign rf_raddr0 = dbg_read ? dbg_addr : rd_src0;
  assign rf_raddr1 = rd_src1;
  assign dbg_data  = rf_rdata0;

  // EXEC writeback (port 1) — computed below; MEM load writeback (port 2).
  warp_vec_t  result_vec;
  warp_mask_t gm_e;
  wire rf_we1 = !stall && re_valid && (is_alu || is_rdsr || is_ldc) && !is_branch;
  wire rf_we2 = m_complete && m_ld;

  regfile #(.NUM_WARPS(NUM_WARPS)) u_rf (
    .clk(clk), .rst(rst),
    .rwarp(dbg_read ? dbg_warp : fr_warp),
    .we(rf_we1),  .wwarp(re_warp), .waddr(rd),   .wdata(result_vec), .wmask(gm_e),
    .we2(rf_we2), .wwarp2(m_warp), .waddr2(m_rd),.wdata2(m_ldvec),   .wmask2(m_gm),
    .ren0(rf_ren0), .raddr0(rf_raddr0), .ren1(rf_ren1), .raddr1(rf_raddr1),
    .ren2(rf_ren1), .raddr2(r_rd),
    .rdata0(rf_rdata0), .rdata1(rf_rdata1), .rdata2(rf_rdata2), .bank_conflict()
  );

  // ---- EXEC-stage masks for the warp at EXEC (its own state can't change in flight) ----
  wire [SPW-1:0] sp_e = sp[re_warp];
  frame_t tos_e;
  assign tos_e = (sp_e == 0) ? '0 : stack[re_warp][sp_e-1];

  warp_mask_t active_e, pred_sel_e, guard_e, ntk_e;
  assign active_e = tos_e.mask & ~exited[re_warp];
  always_comb begin
    unique case (psel)
      2'd0: pred_sel_e = '1;
      2'd1: pred_sel_e = pred1[re_warp];
      2'd2: pred_sel_e = pred2[re_warp];
      2'd3: pred_sel_e = pred3[re_warp];
    endcase
  end
  assign guard_e = gneg ? ~pred_sel_e : pred_sel_e;
  assign gm_e    = active_e & guard_e;
  assign ntk_e   = active_e & ~gm_e;

  // ---- 32 SIMT integer + FP + SFU lanes (operands = registered RF read data) ----
  warp_vec_t alu_y, fp_y, sfu_y;
  logic [2:0] fp_op;
  always_comb begin
    unique case (opcode)
      6'(OP_FADD), 6'(OP_FSUB): fp_op = 3'd0;
      6'(OP_FMUL):              fp_op = 3'd1;
      6'(OP_FFMA):              fp_op = 3'd2;
      6'(OP_FMIN):              fp_op = 3'd3;
      6'(OP_FMAX):              fp_op = 3'd4;
      6'(OP_I2F):               fp_op = 3'd5;
      6'(OP_F2I):               fp_op = 3'd6;
      default:                  fp_op = 3'd7;
    endcase
  end
  genvar l;
  generate
    for (l = 0; l < WARP_SIZE; l++) begin : g_lane
      wire word_t a_l = alu_a_imm ? imm : rf_rdata0[l];
      wire word_t b_l = alu_b_imm ? imm : rf_rdata1[l];
      wire word_t y_l;
      alu u_alu (.opcode(opcode), .a(a_l), .b(b_l), .is_alu(), .y(y_l));
      assign alu_y[l] = y_l;
      // FSUB = FADD with b's sign flipped (b == 0 is caught by the adder's FTZ path)
      wire word_t fb_l = (opcode == OP_FSUB) ? {~rf_rdata1[l][31], rf_rdata1[l][30:0]}
                                             : rf_rdata1[l];
      wire word_t fy_l;
      fp_alu u_fp (.op(fp_op), .a(rf_rdata0[l]), .b(fb_l), .c(rf_rdata2[l]), .y(fy_l));
      assign fp_y[l] = fy_l;
      // SFU lane (M16): MUFU.func rd, rs1
      wire word_t sy_l;
      sfu u_sfu (.func(re_ir[2:0]), .a(rf_rdata0[l]), .y(sy_l));
      assign sfu_y[l] = sy_l;
    end
  endgenerate

  // special registers: thread-in-block = warp*WARP_SIZE + lane.
  function automatic word_t f_sreg(input logic [4:0] sr,
                                   input logic [LANE_ID_W-1:0] lane,
                                   input logic [WW-1:0] wid);
    word_t tib;
    tib = ({{(XLEN-WW){1'b0}}, wid} << LANE_ID_W) | {{(XLEN-LANE_ID_W){1'b0}}, lane};
    unique case (sr)
      5'(SR_TID_X):    f_sreg = (ctx_bdx == 0) ? tib : (tib % ctx_bdx);
      5'(SR_NTID_X):   f_sreg = ctx_bdx;
      5'(SR_NTID_Y):   f_sreg = 32'd1;
      5'(SR_NTID_Z):   f_sreg = 32'd1;
      5'(SR_CTAID_X):  f_sreg = ctx_bidx;
      5'(SR_NCTAID_X): f_sreg = ctx_gdx;
      5'(SR_LANEID):   f_sreg = {{(XLEN-LANE_ID_W){1'b0}}, lane};
      5'(SR_WARPID):   f_sreg = {{(XLEN-WW){1'b0}}, wid};
      5'(SR_TID_FLAT): f_sreg = ctx_bidx * ctx_tpb + tib;
      default:         f_sreg = 32'd0;
    endcase
  endfunction

  function automatic logic f_cmp(input logic [2:0] cc, input word_t a, input word_t b);
    unique case (cc)
      CMP_EQ:  f_cmp = (a == b);
      CMP_NE:  f_cmp = (a != b);
      CMP_LT:  f_cmp = ($signed(a) <  $signed(b));
      CMP_LE:  f_cmp = ($signed(a) <= $signed(b));
      CMP_GT:  f_cmp = ($signed(a) >  $signed(b));
      CMP_GE:  f_cmp = ($signed(a) >= $signed(b));
      CMP_LTU: f_cmp = (a <  b);
      CMP_GEU: f_cmp = (a >= b);
      default: f_cmp = 1'b0;
    endcase
  endfunction

  // float total-order key (mirrors fp_key in isa.hpp) for FSETP
  function automatic word_t f_fkey(input word_t x);
    word_t v;
    begin
      v = (x[30:23] == 8'd0) ? 32'd0 : x;
      f_fkey = v[31] ? ~v : (v | 32'h8000_0000);
    end
  endfunction
  function automatic logic f_fcmp(input logic [2:0] cc, input word_t a, input word_t b);
    word_t ka, kb;
    begin
      ka = f_fkey(a); kb = f_fkey(b);
      unique case (cc)
        CMP_EQ:  f_fcmp = (ka == kb);
        CMP_NE:  f_fcmp = (ka != kb);
        CMP_LT:  f_fcmp = (ka <  kb);
        CMP_LE:  f_fcmp = (ka <= kb);
        CMP_GT:  f_fcmp = (ka >  kb);
        CMP_GE:  f_fcmp = (ka >= kb);
        default: f_fcmp = 1'b0;      // LTU/GEU reserved for floats
      endcase
    end
  endfunction

  warp_mask_t setp_vec;
  always_comb begin
    for (int i = 0; i < WARP_SIZE; i++) begin
      result_vec[i] = is_rdsr  ? f_sreg(imm[4:0], i[LANE_ID_W-1:0], re_warp)
                    : (is_ldc  ? cbank[imm[CBW-1:0]]
                    : (is_falu ? fp_y[i]
                    : (is_mufu ? sfu_y[i] : alu_y[i])));
      setp_vec[i]   = is_fsetp ? f_fcmp(cmp_code, rf_rdata0[i], rf_rdata1[i])
                               : f_cmp(cmp_code, rf_rdata0[i], is_setpi ? imm : rf_rdata1[i]);
    end
  end

  // ---- branch/reconvergence helpers (EXEC warp) ----
  wire [PCW-1:0] br_target = re_pc + imm[PCW-1:0];
  wire [PCW-1:0] recon_R   = (tos_e.pend != DONE) ? tos_e.pend : tos_e.rpc;

  // atomic RMW function (mirrors atom_ref in isa.hpp; used for shared-memory atomics)
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

  // shared-memory write (STS store, or the write half of a shared atomic RMW)
  always_ff @(posedge clk)
    if (m_step) begin
      if (m_atom)
        smem[m_word_s] <= f_atom(m_afunc, smem[m_word_s], m_sdata[m_lane], m_cmpv[m_lane]);
      else if (!m_ld)
        smem[m_word_s] <= m_sdata[m_lane];
    end

  // pack resident_w (unpacked array) into a vector for the launch assignment
  function automatic logic [NUM_WARPS-1:0] resident_w_vec();
    for (int w = 0; w < NUM_WARPS; w++) resident_w_vec[w] = resident_w[w];
  endfunction

  // ================================================================================
  // Sequential control: launch, PICK/FETCH/READ/EXEC advance, memory unit, barrier
  // ================================================================================
  always_ff @(posedge clk) begin
    if (rst) begin
      running <= 1'b0; block_done <= 1'b0;
      pf_valid <= 1'b0; fr_valid <= 1'b0; re_valid <= 1'b0;
      pf_warp <= '0; pf_pc <= '0; fr_warp <= '0; fr_pc <= '0; fr_ir <= '0;
      re_warp <= '0; re_pc <= '0; re_ir <= '0;
      m_busy <= 1'b0; m_ld <= 1'b0; m_shared <= 1'b0; m_warp <= '0; m_rd <= '0;
      m_imm <= '0; m_gm <= '0; m_pending <= '0; m_base <= '0; m_sdata <= '0; m_ldvec <= '0;
      m_atom <= 1'b0; m_afunc <= '0; m_cmpv <= '0;
      c_wait <= 1'b0; c_match <= '0;
      finished <= '1; at_barrier <= '0; resident <= '0; in_flight <= '0;
      perf_cycles <= '0; perf_insns <= '0; perf_memops <= '0;
      for (int w = 0; w < NUM_WARPS; w++) begin
        sp[w] <= '0; exited[w] <= '0; pred1[w] <= '0; pred2[w] <= '0; pred3[w] <= '0;
      end
    end else begin
      if (running) perf_cycles <= perf_cycles + 1;

      // ---- launch a whole block ----
      if (!running && start) begin
        for (int w = 0; w < NUM_WARPS; w++) begin
          stack[w][0] <= '{mask: present_w[w], pc: {1'b0, entry}, rpc: DONE, pend: DONE};
          sp[w]       <= resident_w[w] ? {{(SPW-1){1'b0}}, 1'b1} : '0;
          exited[w]   <= ~present_w[w];
          pred1[w] <= '0; pred2[w] <= '0; pred3[w] <= '0;
        end
        finished   <= ~resident_w_vec();   // non-resident warps never run
        at_barrier <= '0;
        resident   <= resident_w_vec();
        in_flight  <= '0;
        pf_valid <= 1'b0; fr_valid <= 1'b0; re_valid <= 1'b0;
        block_done <= 1'b0;
        running <= 1'b1;
      end else if (running) begin
        // ---------------- memory unit (before the pipeline blocks: a same-cycle
        // completion + new EXEC dispatch must leave the DISPATCH values in m_*) --------
        if (m_step) begin
          // shared memory: serial active-lane walk on the scratchpad (loads + atomics
          // capture the OLD word; the write half happens in the smem block above)
          if (m_ld) m_ldvec[m_lane] <= smem[m_word_s];
          m_pending[m_lane] <= 1'b0;
        end
        if (g_req) begin
          // global memory: one coalesced line transaction (or one atomic word) on the L1
          c_match <= g_match;
          c_wait  <= 1'b1;
        end
        if (c_wait && l1_resp_ready) begin
          if (m_atom) begin
            // atomic: the single lane of the transaction receives the OLD value
            for (int i = 0; i < WARP_SIZE; i++)
              if (c_match[i]) m_ldvec[i] <= l1_resp_old;
          end else if (m_ld) begin
            // load: scatter the returned line to every lane of the transaction
            for (int i = 0; i < WARP_SIZE; i++)
              if (c_match[i])
                m_ldvec[i] <= l1_resp_rline[lane_byte[i][2+OFFW-1:2]*XLEN +: XLEN];
          end
          m_pending <= m_pending & ~c_match;
          c_wait    <= 1'b0;
        end
        if (m_complete) begin
          m_busy <= 1'b0;
          in_flight[m_warp] <= 1'b0;   // (load writeback happens this cycle on RF port 2)
        end

        // ---------------- PICK ----------------
        if (!stall) begin
          if (pick_fin)  finished[sched_sel] <= 1'b1;
          if (pick_pop)  sp[sched_sel] <= sp_p - 1'b1;
          if (pick_issue) begin
            in_flight[sched_sel] <= 1'b1;
            pf_valid <= 1'b1; pf_warp <= sched_sel; pf_pc <= tos_p.pc;
          end else begin
            pf_valid <= 1'b0;
          end

          // ---------------- FETCH -> READ ----------------
          fr_valid <= pf_valid; fr_warp <= pf_warp; fr_pc <= pf_pc;
          fr_ir    <= imem[pf_pc[AW-1:0]];

          // ---------------- READ -> EXEC ----------------
          re_valid <= fr_valid; re_warp <= fr_warp; re_pc <= fr_pc; re_ir <= fr_ir;

          // ---------------- EXEC (retire) ----------------
          if (re_valid) begin
            perf_insns <= perf_insns + 1;
            if (is_setp) begin
              unique case (pd)
                2'd1: pred1[re_warp] <= (pred1[re_warp] & ~gm_e) | (setp_vec & gm_e);
                2'd2: pred2[re_warp] <= (pred2[re_warp] & ~gm_e) | (setp_vec & gm_e);
                2'd3: pred3[re_warp] <= (pred3[re_warp] & ~gm_e) | (setp_vec & gm_e);
                default: ;
              endcase
            end

            if (is_mem_loop) begin
              // hand to the memory unit (free by construction: !stall)
              perf_memops <= perf_memops + 1;
              m_busy   <= 1'b1;
              m_ld     <= is_ld_mem | is_atom;   // atomics write the OLD value back to rd
              m_atom   <= is_atom;
              m_afunc  <= re_ir[10:8];
              m_shared <= is_shared;
              m_warp   <= re_warp;
              m_rd     <= rd;
              m_imm    <= imm;
              m_gm     <= gm_e;
              m_pending<= gm_e;
              m_base   <= rf_rdata0;
              m_sdata  <= rf_rdata1;
              m_cmpv   <= rf_rdata2;             // CAS compare values (rd, via read port 2)
              c_wait   <= 1'b0;
              stack[re_warp][sp_e-1].pc <= re_pc + 1'b1;
              // in_flight stays set until the memory unit completes
            end else if (is_bar) begin
              at_barrier[re_warp] <= 1'b1;             // park at the barrier
              stack[re_warp][sp_e-1].pc <= re_pc + 1'b1;
              in_flight[re_warp] <= 1'b0;
            end else begin
              in_flight[re_warp] <= 1'b0;
              if (is_branch) begin
                if (gm_e == '0)        stack[re_warp][sp_e-1].pc <= re_pc + 1'b1;
                else if (ntk_e == '0)  stack[re_warp][sp_e-1].pc <= br_target;
                else begin
                  stack[re_warp][sp_e-1] <= '{mask: active_e, pc: recon_R,      rpc: tos_e.rpc, pend: DONE};
                  stack[re_warp][sp_e]   <= '{mask: ntk_e,    pc: re_pc + 1'b1, rpc: recon_R,   pend: DONE};
                  stack[re_warp][sp_e+1] <= '{mask: gm_e,     pc: br_target,    rpc: recon_R,   pend: DONE};
                  sp[re_warp] <= sp_e + 2'd2;
                end
              end else if (is_ssy) begin
                stack[re_warp][sp_e-1].pend <= br_target;
                stack[re_warp][sp_e-1].pc   <= re_pc + 1'b1;
              end else if (is_sync) begin
                stack[re_warp][sp_e-1].pc <= tos_e.rpc;
              end else if (is_exit) begin
                exited[re_warp] <= exited[re_warp] | gm_e;
                stack[re_warp][sp_e-1].pc <= re_pc + 1'b1;
              end else begin
                stack[re_warp][sp_e-1].pc <= re_pc + 1'b1;
              end
            end
          end
        end

        // ---------------- barrier release / block completion ----------------
        if (!any_ready && !any_inflight) begin
          if (any_parked) begin
            at_barrier <= at_barrier & ~(resident & ~finished);  // release all live warps
          end else begin
            block_done <= 1'b1;
            running    <= 1'b0;
            pf_valid <= 1'b0; fr_valid <= 1'b0; re_valid <= 1'b0;
          end
        end
      end
    end
  end

  // ----------------------------------------------------------------------------
  // Assertions (SVA)
  // ----------------------------------------------------------------------------
`ifndef SYNTHESIS
  a_stack_bounds:  assert property (@(posedge clk) disable iff (rst)
                     re_valid |-> sp_e <= STACK_DEPTH);
  a_exec_inflight: assert property (@(posedge clk) disable iff (rst)
                     (re_valid && running) |-> in_flight[re_warp]);
  a_mem_inflight:  assert property (@(posedge clk) disable iff (rst)
                     (m_busy && running) |-> in_flight[m_warp]);
  genvar gw;
  generate for (gw = 0; gw < NUM_WARPS; gw++) begin : g_asrt
    a_bar_fin:   assert property (@(posedge clk) disable iff (rst)
                   !(finished[gw] && at_barrier[gw]));
    a_ready_iff: assert property (@(posedge clk) disable iff (rst)
                   !(sched_ready[gw] && in_flight[gw]));
  end endgenerate
`endif

endmodule

`default_nettype wire
