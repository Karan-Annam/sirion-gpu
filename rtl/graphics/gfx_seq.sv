// gfx_seq.sv — graphics command processor (M20).
//
// Executes ONE DRAW CALL entirely in hardware, sequencing the programmable pipeline the
// host had to drive by hand in M19:
//
//   draw ──► launch VERTEX-SHADER grid on the CUs ──► wait
//        ──► per triangle: fetch 3 indices + 3 transformed vertices from global memory,
//            feed the rasterizer in fragment-EMIT mode ──► wait
//        ──► DMA the fragment records into global memory (the FS's input buffer)
//        ──► write the FS kernel parameters (c[0]=fbuf, c[1]=nfrag, c[2]=texture)
//        ──► launch FRAGMENT-SHADER grid ──► wait
//        ──► stream shaded fragments into the ROP (depth test + blend + write)
//        ──► draw_done
//
// The VS and FS binaries are BOTH resident in instruction memory (different entry points;
// Sirion branches are PC-relative so programs are position-independent). VS parameters are
// host-set before the draw; the sequencer overwrites c[0..2] for the FS after the VS has
// finished. FS blocks are fixed at 256 threads, so the grid size is (nfrag+255)>>8.
//
// Global memory is accessed through the host load/readback ports (gdbg combinational read,
// gmem_we synchronous write), which are free while the sequencer owns the device — the
// same access path the host driver uses, muxed in gpu_gfx_top.
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module gfx_seq
  import sirion_pkg::*;
#(
  parameter int IMEM_WORDS = 512,
  parameter int GMEM_WORDS = 65536,
  parameter int RW = 96,
  parameter int RH = 96,
  parameter int FRAG_MAX = 16384,
  localparam int GAW = $clog2(GMEM_WORDS)
) (
  input  wire         clk,
  input  wire         rst,
  // ---- draw command (descriptor held stable; `draw` is a 1-cycle pulse) ----
  input  wire         draw,
  input  wire  word_t ds_nverts,     // vertices for the VS launch (<= 256, one block)
  input  wire  word_t ds_ntris,      // triangles in the index buffer
  input  wire  word_t ds_vout,       // transformed-vertex buffer (byte addr, 24 B/vertex)
  input  wire  word_t ds_ibuf,       // index buffer (byte addr, 3 words per triangle)
  input  wire  word_t ds_fbuf,       // fragment buffer for the FS (byte addr, 12 B/record)
  input  wire  word_t ds_tex,        // texture base for the FS (byte addr)
  input  wire [$clog2(IMEM_WORDS)-1:0] ds_vs_entry,
  input  wire [$clog2(IMEM_WORDS)-1:0] ds_fs_entry,
  input  wire         ds_blend,
  input  wire [7:0]   ds_alpha,
  input  wire         ds_dwrite,
  output logic        draw_busy,
  output logic        draw_done,     // 1-cycle pulse
  // ---- compute-GPU control (muxed onto gpu_top in gpu_gfx_top) ----
  output logic        g_launch,
  output word_t       g_grid,
  output word_t       g_tpb,
  output logic [$clog2(IMEM_WORDS)-1:0] g_entry,
  input  wire         g_done,        // grid_done pulse
  output logic        g_cbank_we,
  output logic [5:0]  g_cbank_waddr,
  output word_t       g_cbank_wdata,
  // global-memory access (host-port mux)
  output logic [GAW-1:0] g_raddr,    // combinational read address (word)
  input  wire  word_t    g_rdata,
  output logic           g_we,
  output logic [GAW-1:0] g_waddr,
  output word_t          g_wdata,
  // ---- rasterizer control (muxed onto raster in gpu_gfx_top) ----
  output logic        r_frag_rst,    // pulse: new draw, zero the fragment count
  output logic        r_tri_valid,
  output logic signed [31:0] r_x [3],
  output logic signed [31:0] r_y [3],
  output logic signed [31:0] r_z [3],
  output logic signed [31:0] r_q [3],
  output logic [7:0]  r_r [3], r_g [3], r_b [3],
  output logic signed [31:0] r_u [3], r_v [3],
  output logic        r_emit,
  input  wire         r_busy,
  input  wire [31:0]  r_frag_count,
  output logic [$clog2(FRAG_MAX)-1:0] r_fdbg_idx,
  input  wire [31:0]  r_fdbg_w0,
  input  wire [31:0]  r_fdbg_w1,
  input  wire [31:0]  r_fdbg_w2,
  output logic        r_fin_valid,
  output logic [$clog2(RW*RH)-1:0] r_fin_pidx,
  output logic signed [31:0]       r_fin_z,
  output logic [23:0]              r_fin_color,
  output logic        r_blend_en,
  output logic [7:0]  r_alpha,
  output logic        r_depth_write
);

  typedef enum logic [3:0] {
    S_IDLE, S_VS_GO, S_VS_WAIT, S_TRI_IDX, S_TRI_VTX, S_TRI_GO, S_TRI_WAIT,
    S_DMA, S_FS_CB, S_FS_GO, S_FS_WAIT, S_ROP_R0, S_ROP_R2, S_ROP_GO, S_DONE
  } state_e;
  state_e state;

  // latched descriptor
  word_t q_nverts, q_ntris, q_vout, q_ibuf, q_fbuf, q_tex;
  logic [$clog2(IMEM_WORDS)-1:0] q_vse, q_fse;
  logic q_blend, q_dwrite;
  logic [7:0] q_alpha;

  word_t tri_i;                 // triangle index
  logic [1:0] vsel;           // which of the 3 vertices
  logic [2:0] wsel;           // which of the 6 vertex words
  word_t idx [3];             // fetched vertex indices
  word_t vw  [3][6];          // fetched vertex words: x y z q rgb uvp
  word_t dma_k;               // fragment-DMA word counter
  logic [1:0] cb_k;           // FS constant counter
  word_t rop_k;               // ROP record counter
  word_t rop_w0;

  assign draw_busy = (state != S_IDLE);
  assign r_blend_en    = q_blend;
  assign r_alpha       = q_alpha;
  assign r_depth_write = q_dwrite;
  assign r_emit        = 1'b1;   // sequencer triangles always emit fragments

  // triangle vertex attribute unpack (combinational from vw)
  always_comb begin
    for (int j = 0; j < 3; j++) begin
      r_x[j] = vw[j][0]; r_y[j] = vw[j][1]; r_z[j] = vw[j][2]; r_q[j] = vw[j][3];
      r_r[j] = vw[j][4][23:16]; r_g[j] = vw[j][4][15:8]; r_b[j] = vw[j][4][7:0];
      r_u[j] = {16'd0, vw[j][5][15:0]};
      r_v[j] = {16'd0, vw[j][5][31:16]};
    end
  end

  // combinational read address per state
  always_comb begin
    g_raddr = '0;
    unique case (state)
      S_TRI_IDX: g_raddr = GAW'((q_ibuf >> 2) + tri_i*3 + word_t'(vsel));
      S_TRI_VTX: g_raddr = GAW'((q_vout >> 2) + idx[vsel]*6 + word_t'(wsel));
      S_ROP_R0:  g_raddr = GAW'((q_fbuf >> 2) + rop_k*3);
      S_ROP_R2:  g_raddr = GAW'((q_fbuf >> 2) + rop_k*3 + 2);
      default: ;
    endcase
  end

  // fragment-DMA: word dma_k of the packed record stream (3 words per record)
  wire word_t dma_rec = dma_k / 3;
  wire [1:0]  dma_sel = 2'(dma_k % 3);
  assign r_fdbg_idx = dma_rec[$clog2(FRAG_MAX)-1:0];
  wire word_t dma_word = (dma_sel == 0) ? r_fdbg_w0 : (dma_sel == 1) ? r_fdbg_w1 : r_fdbg_w2;

  always_ff @(posedge clk) begin
    if (rst) begin
      state <= S_IDLE; draw_done <= 1'b0;
      g_launch <= 1'b0; g_grid <= '0; g_tpb <= '0; g_entry <= '0;
      g_cbank_we <= 1'b0; g_cbank_waddr <= '0; g_cbank_wdata <= '0;
      g_we <= 1'b0; g_waddr <= '0; g_wdata <= '0;
      r_tri_valid <= 1'b0; r_fin_valid <= 1'b0; r_frag_rst <= 1'b0;
      r_fin_pidx <= '0; r_fin_z <= '0; r_fin_color <= '0;
      q_nverts <= '0; q_ntris <= '0; q_vout <= '0; q_ibuf <= '0; q_fbuf <= '0; q_tex <= '0;
      q_vse <= '0; q_fse <= '0; q_blend <= 1'b0; q_dwrite <= 1'b1; q_alpha <= 8'd255;
      tri_i <= '0; vsel <= '0; wsel <= '0; dma_k <= '0; cb_k <= '0; rop_k <= '0; rop_w0 <= '0;
      for (int j = 0; j < 3; j++) begin idx[j] <= '0; for (int w = 0; w < 6; w++) vw[j][w] <= '0; end
    end else begin
      draw_done   <= 1'b0;
      g_launch    <= 1'b0;
      g_cbank_we  <= 1'b0;
      g_we        <= 1'b0;
      r_tri_valid <= 1'b0;
      r_fin_valid <= 1'b0;
      r_frag_rst  <= 1'b0;

      unique case (state)
        S_IDLE: if (draw) begin
          r_frag_rst <= 1'b1;                      // fresh fragment buffer for this draw
          q_nverts <= ds_nverts; q_ntris <= ds_ntris;
          q_vout <= ds_vout; q_ibuf <= ds_ibuf; q_fbuf <= ds_fbuf; q_tex <= ds_tex;
          q_vse <= ds_vs_entry; q_fse <= ds_fs_entry;
          q_blend <= ds_blend; q_alpha <= ds_alpha; q_dwrite <= ds_dwrite;
          state <= S_VS_GO;
        end

        // ---- vertex shading on the CUs ----
        S_VS_GO: begin
          g_launch <= 1'b1; g_grid <= 32'd1; g_tpb <= q_nverts; g_entry <= q_vse;
          state <= S_VS_WAIT;
        end
        S_VS_WAIT: if (g_done) begin
          tri_i <= '0; vsel <= 2'd0; state <= S_TRI_IDX;
        end

        // ---- per-triangle: fetch indices, fetch vertices, rasterize (emit) ----
        S_TRI_IDX: begin
          idx[vsel] <= g_rdata;
          if (vsel == 2'd2) begin vsel <= 2'd0; wsel <= 3'd0; state <= S_TRI_VTX; end
          else vsel <= vsel + 2'd1;
        end
        S_TRI_VTX: begin
          vw[vsel][wsel] <= g_rdata;
          if (wsel == 3'd5) begin
            wsel <= 3'd0;
            if (vsel == 2'd2) state <= S_TRI_GO;
            else vsel <= vsel + 2'd1;
          end else wsel <= wsel + 3'd1;
        end
        S_TRI_GO: begin
          r_tri_valid <= 1'b1;
          state <= S_TRI_WAIT;
        end
        S_TRI_WAIT: if (!r_busy && !r_tri_valid) begin
          if (tri_i + 1 >= q_ntris) begin dma_k <= '0; state <= S_DMA; end
          else begin tri_i <= tri_i + 1; vsel <= 2'd0; state <= S_TRI_IDX; end
        end

        // ---- DMA fragment records into global memory ----
        S_DMA: begin
          if (dma_k >= r_frag_count * 3) begin cb_k <= 2'd0; state <= S_FS_CB; end
          else begin
            g_we    <= 1'b1;
            g_waddr <= GAW'((q_fbuf >> 2) + dma_k);
            g_wdata <= dma_word;
            dma_k   <= dma_k + 1;
          end
        end

        // ---- FS parameters + launch ----
        S_FS_CB: begin
          g_cbank_we    <= 1'b1;
          g_cbank_waddr <= 6'(cb_k);
          g_cbank_wdata <= (cb_k == 0) ? q_fbuf : (cb_k == 1) ? r_frag_count : q_tex;
          if (cb_k == 2'd2) state <= S_FS_GO;
          else cb_k <= cb_k + 2'd1;
        end
        S_FS_GO: begin
          g_launch <= 1'b1;
          g_grid   <= (r_frag_count + 32'd255) >> 8;
          g_tpb    <= 32'd256;
          g_entry  <= q_fse;
          state <= S_FS_WAIT;
        end
        S_FS_WAIT: if (g_done) begin
          rop_k <= '0; state <= S_ROP_R0;
        end

        // ---- ROP: stream shaded fragments (2 reads + 1 submit per record) ----
        S_ROP_R0: begin
          if (rop_k >= r_frag_count) state <= S_DONE;
          else begin rop_w0 <= g_rdata; state <= S_ROP_R2; end
        end
        S_ROP_R2: begin
          r_fin_pidx  <= rop_w0[$clog2(RW*RH)-1:0];
          r_fin_z     <= {16'd0, rop_w0[31:16]};
          r_fin_color <= g_rdata[23:0];
          r_fin_valid <= 1'b1;
          state <= S_ROP_GO;
        end
        S_ROP_GO: begin
          rop_k <= rop_k + 1;
          state <= S_ROP_R0;
        end

        S_DONE: begin
          draw_done <= 1'b1;
          state <= S_IDLE;
        end
      endcase
    end
  end

`ifndef SYNTHESIS
  a_done_pulse: assert property (@(posedge clk) disable iff (rst) draw_done |=> !draw_done);
`endif

endmodule

`default_nettype wire
