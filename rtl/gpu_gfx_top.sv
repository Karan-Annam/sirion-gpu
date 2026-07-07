// gpu_gfx_top.sv — compute GPU + rasterizer, one device (M19).
//
// Packages the programmable compute machine (gpu_top: CUs -> L1s -> L2 -> global memory)
// together with the fixed-function rasterizer so PROGRAMMABLE SHADING can run end-to-end
// on one DUT: the vertex and fragment shaders execute as kernels on the CU array, and the
// rasterizer supplies coverage/interpolation (fragment EMIT mode) and the ROP (fragment
// input port). In M19 the host moves the small vertex/fragment buffers between the two
// engines' memories; M20 gives the rasterizer its own port on the memory system and a
// command processor to sequence passes without the host.
//
// Raster-side ports carry an r_ prefix; compute-side ports keep their gpu_top names.
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module gpu_gfx_top
  import sirion_pkg::*;
#(
  parameter  int NUM_CU      = 1,
  parameter  int IMEM_WORDS  = 1024,
  parameter  int CBANK_WORDS = 64,
  parameter  int GMEM_WORDS  = 65536,   // 256 KB: vertex/fragment/texture buffers live here
  parameter  int NUM_WARPS   = 8,
  parameter  int RW          = 96,      // render target (kept small for simulation speed)
  parameter  int RH          = 96,
  parameter  int TEX_W       = 32,
  parameter  int TEX_H       = 32,
  parameter  int FRAG_MAX    = 16384,
  localparam int WW = (NUM_WARPS > 1) ? $clog2(NUM_WARPS) : 1,
  localparam int CW = (NUM_CU > 1) ? $clog2(NUM_CU) : 1
) (
  input  wire                          clk,
  input  wire                          rst,
  // ---- compute side (see gpu_top.sv) ----
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
  input  wire                          launch,
  input  wire  word_t                  grid_nx,
  input  wire  word_t                  tpb,
  input  wire [$clog2(IMEM_WORDS)-1:0] entry,
  output logic                         grid_busy,
  output logic                         grid_done,
  output logic [31:0]                  perf_blocks,
  output logic [31:0]                  perf_cycles,
  output logic [31:0]                  perf_insns,
  output logic [31:0]                  perf_memops,
  output logic [31:0]                  perf_l1_hits,
  output logic [31:0]                  perf_l1_misses,
  output logic [31:0]                  perf_l2_hits,
  output logic [31:0]                  perf_l2_misses,
  input  wire  [CW-1:0]                dbg_cu,
  input  wire  [WW-1:0]                dbg_warp,
  input  wire  vreg_id_t               dbg_addr,
  output warp_vec_t                    dbg_data,
  output warp_mask_t                   dbg_pred1,
  output warp_mask_t                   dbg_pred2,
  output warp_mask_t                   dbg_pred3,
  // ---- raster side (see raster.sv; r_ prefix) ----
  input  wire        r_clear,
  input  wire [23:0] r_clear_color,
  input  wire        r_tri_valid,
  input  wire signed [31:0] r_x0, r_y0, r_z0, r_x1, r_y1, r_z1, r_x2, r_y2, r_z2,
  input  wire [7:0]  r_r0, r_g0, r_b0, r_r1, r_g1, r_b1, r_r2, r_g2, r_b2,
  input  wire signed [31:0] r_q0, r_q1, r_q2,
  input  wire        r_cull_backface,
  input  wire        r_textured,
  input  wire        r_tex_bilinear,
  input  wire        r_tex_mip,
  input  wire signed [31:0] r_u0, r_v0, r_u1, r_v1, r_u2, r_v2,
  input  wire        r_tex_we,
  input  wire [$clog2(TEX_W*TEX_H*2)-1:0] r_tex_waddr,
  input  wire [23:0] r_tex_wdata,
  input  wire        r_blend_en,
  input  wire [7:0]  r_alpha,
  input  wire        r_depth_write,
  input  wire        r_emit_mode,
  output logic [31:0] r_frag_count,
  input  wire [$clog2(FRAG_MAX)-1:0] r_fdbg_idx,
  output logic [31:0] r_fdbg_w0,
  output logic [31:0] r_fdbg_w1,
  output logic [31:0] r_fdbg_w2,
  input  wire        r_fin_valid,
  input  wire [$clog2(RW*RH)-1:0] r_fin_pidx,
  input  wire signed [31:0]       r_fin_z,
  input  wire [23:0]              r_fin_color,
  output logic       r_busy,
  input  wire [$clog2(RW*RH)-1:0] r_fb_raddr,
  output logic [23:0]             r_fb_rcolor,
  output logic [31:0]             r_perf_pixels,
  output logic [31:0]             r_perf_frags,
  // ---- M20: hardware draw-call command (gfx_seq) ----
  input  wire        draw,          // 1-cycle pulse; descriptor held stable
  input  wire  word_t ds_nverts,
  input  wire  word_t ds_ntris,
  input  wire  word_t ds_vout,
  input  wire  word_t ds_ibuf,
  input  wire  word_t ds_fbuf,
  input  wire  word_t ds_tex,
  input  wire [$clog2(IMEM_WORDS)-1:0] ds_vs_entry,
  input  wire [$clog2(IMEM_WORDS)-1:0] ds_fs_entry,
  input  wire        ds_blend,
  input  wire [7:0]  ds_alpha,
  input  wire        ds_dwrite,
  output logic       draw_busy,
  output logic       draw_done
);

  // ---- graphics command processor (M20) + host/sequencer muxes ----
  logic        s_launch;
  word_t       s_grid, s_tpb;
  logic [$clog2(IMEM_WORDS)-1:0] s_entry;
  logic        s_cb_we;
  logic [5:0]  s_cb_wa;
  word_t       s_cb_wd;
  logic [$clog2(GMEM_WORDS)-1:0] s_raddr, s_waddr;
  logic        s_we;
  word_t       s_wdata;
  logic        s_tri_valid, s_emit, s_fin_valid, s_frag_rst;
  logic signed [31:0] s_x [3], s_y [3], s_z [3], s_q [3], s_u [3], s_v [3];
  logic [7:0]  s_r [3], s_g [3], s_b [3];
  logic [$clog2(FRAG_MAX)-1:0]  s_fdbg_idx;
  logic [$clog2(RW*RH)-1:0]     s_fin_pidx;
  logic signed [31:0]           s_fin_z;
  logic [23:0]                  s_fin_color;
  logic        s_blend;
  logic [7:0]  s_alpha;
  logic        s_dwrite;
  logic        grid_done_i;
  word_t       gdbg_data_i;

  gfx_seq #(
    .IMEM_WORDS(IMEM_WORDS), .GMEM_WORDS(GMEM_WORDS),
    .RW(RW), .RH(RH), .FRAG_MAX(FRAG_MAX)
  ) u_seq (
    .clk(clk), .rst(rst),
    .draw(draw), .ds_nverts(ds_nverts), .ds_ntris(ds_ntris),
    .ds_vout(ds_vout), .ds_ibuf(ds_ibuf), .ds_fbuf(ds_fbuf), .ds_tex(ds_tex),
    .ds_vs_entry(ds_vs_entry), .ds_fs_entry(ds_fs_entry),
    .ds_blend(ds_blend), .ds_alpha(ds_alpha), .ds_dwrite(ds_dwrite),
    .draw_busy(draw_busy), .draw_done(draw_done),
    .g_launch(s_launch), .g_grid(s_grid), .g_tpb(s_tpb), .g_entry(s_entry),
    .g_done(grid_done_i),
    .g_cbank_we(s_cb_we), .g_cbank_waddr(s_cb_wa), .g_cbank_wdata(s_cb_wd),
    .g_raddr(s_raddr), .g_rdata(gdbg_data_i),
    .g_we(s_we), .g_waddr(s_waddr), .g_wdata(s_wdata),
    .r_frag_rst(s_frag_rst),
    .r_tri_valid(s_tri_valid),
    .r_x(s_x), .r_y(s_y), .r_z(s_z), .r_q(s_q),
    .r_r(s_r), .r_g(s_g), .r_b(s_b), .r_u(s_u), .r_v(s_v),
    .r_emit(s_emit), .r_busy(r_busy), .r_frag_count(r_frag_count),
    .r_fdbg_idx(s_fdbg_idx), .r_fdbg_w0(r_fdbg_w0), .r_fdbg_w1(r_fdbg_w1), .r_fdbg_w2(r_fdbg_w2),
    .r_fin_valid(s_fin_valid), .r_fin_pidx(s_fin_pidx), .r_fin_z(s_fin_z),
    .r_fin_color(s_fin_color),
    .r_blend_en(s_blend), .r_alpha(s_alpha), .r_depth_write(s_dwrite)
  );

  gpu_top #(
    .NUM_CU(NUM_CU), .IMEM_WORDS(IMEM_WORDS), .CBANK_WORDS(CBANK_WORDS),
    .GMEM_WORDS(GMEM_WORDS), .NUM_WARPS(NUM_WARPS)
  ) u_gpu (
    .clk(clk), .rst(rst),
    .imem_we(imem_we), .imem_waddr(imem_waddr), .imem_wdata(imem_wdata),
    .cbank_we(draw_busy ? s_cb_we : cbank_we),
    .cbank_waddr(draw_busy ? s_cb_wa : cbank_waddr),
    .cbank_wdata(draw_busy ? s_cb_wd : cbank_wdata),
    .gmem_we(draw_busy ? s_we : gmem_we),
    .gmem_waddr(draw_busy ? s_waddr : gmem_waddr),
    .gmem_wdata(draw_busy ? s_wdata : gmem_wdata),
    .gdbg_addr(draw_busy ? s_raddr : gdbg_addr), .gdbg_data(gdbg_data_i),
    .launch(draw_busy ? s_launch : launch),
    .grid_nx(draw_busy ? s_grid : grid_nx),
    .tpb(draw_busy ? s_tpb : tpb),
    .entry(draw_busy ? s_entry : entry),
    .grid_busy(grid_busy), .grid_done(grid_done_i),
    .perf_blocks(perf_blocks), .perf_cycles(perf_cycles), .perf_insns(perf_insns),
    .perf_memops(perf_memops), .perf_l1_hits(perf_l1_hits), .perf_l1_misses(perf_l1_misses),
    .perf_l2_hits(perf_l2_hits), .perf_l2_misses(perf_l2_misses),
    .dbg_cu(dbg_cu), .dbg_warp(dbg_warp), .dbg_addr(dbg_addr), .dbg_data(dbg_data),
    .dbg_pred1(dbg_pred1), .dbg_pred2(dbg_pred2), .dbg_pred3(dbg_pred3)
  );
  assign gdbg_data = gdbg_data_i;
  assign grid_done = grid_done_i;

  raster #(.W(RW), .H(RH), .TEX_W(TEX_W), .TEX_H(TEX_H), .FRAG_MAX(FRAG_MAX)) u_raster (
    .clk(clk), .rst(rst),
    .clear(r_clear), .clear_color(r_clear_color),
    .tri_valid(draw_busy ? s_tri_valid : r_tri_valid),
    .x0(draw_busy ? s_x[0] : r_x0), .y0(draw_busy ? s_y[0] : r_y0), .z0(draw_busy ? s_z[0] : r_z0),
    .x1(draw_busy ? s_x[1] : r_x1), .y1(draw_busy ? s_y[1] : r_y1), .z1(draw_busy ? s_z[1] : r_z1),
    .x2(draw_busy ? s_x[2] : r_x2), .y2(draw_busy ? s_y[2] : r_y2), .z2(draw_busy ? s_z[2] : r_z2),
    .r0(draw_busy ? s_r[0] : r_r0), .g0(draw_busy ? s_g[0] : r_g0), .b0(draw_busy ? s_b[0] : r_b0),
    .r1(draw_busy ? s_r[1] : r_r1), .g1(draw_busy ? s_g[1] : r_g1), .b1(draw_busy ? s_b[1] : r_b1),
    .r2(draw_busy ? s_r[2] : r_r2), .g2(draw_busy ? s_g[2] : r_g2), .b2(draw_busy ? s_b[2] : r_b2),
    .q0(draw_busy ? s_q[0] : r_q0), .q1(draw_busy ? s_q[1] : r_q1), .q2(draw_busy ? s_q[2] : r_q2),
    .cull_backface(draw_busy ? 1'b0 : r_cull_backface),
    .textured(draw_busy ? 1'b0 : r_textured),
    .tex_bilinear(r_tex_bilinear), .tex_mip(r_tex_mip),
    .u0(draw_busy ? s_u[0] : r_u0), .v0(draw_busy ? s_v[0] : r_v0),
    .u1(draw_busy ? s_u[1] : r_u1), .v1(draw_busy ? s_v[1] : r_v1),
    .u2(draw_busy ? s_u[2] : r_u2), .v2(draw_busy ? s_v[2] : r_v2),
    .tex_we(r_tex_we), .tex_waddr(r_tex_waddr), .tex_wdata(r_tex_wdata),
    .blend_en(draw_busy ? s_blend : r_blend_en),
    .alpha(draw_busy ? s_alpha : r_alpha),
    .depth_write(draw_busy ? s_dwrite : r_depth_write),
    .emit_mode(draw_busy ? s_emit : r_emit_mode),
    .frag_rst(draw_busy ? s_frag_rst : 1'b0), .frag_count(r_frag_count),
    .fdbg_idx(draw_busy ? s_fdbg_idx : r_fdbg_idx),
    .fdbg_w0(r_fdbg_w0), .fdbg_w1(r_fdbg_w1), .fdbg_w2(r_fdbg_w2),
    .fin_valid(draw_busy ? s_fin_valid : r_fin_valid),
    .fin_pidx(draw_busy ? s_fin_pidx : r_fin_pidx),
    .fin_z(draw_busy ? s_fin_z : r_fin_z),
    .fin_color(draw_busy ? s_fin_color : r_fin_color),
    .busy(r_busy),
    .fb_raddr(r_fb_raddr), .fb_rcolor(r_fb_rcolor),
    .perf_pixels(r_perf_pixels), .perf_frags(r_perf_frags)
  );

endmodule

`default_nettype wire
