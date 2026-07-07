// raster.sv — Sirion fixed-function triangle rasterizer (M9, extended M18).
//
// Triangle setup + edge-equation coverage + PERSPECTIVE-CORRECT attribute interpolation +
// texturing (nearest or BILINEAR, optional per-triangle MIP level) + ROP (depth test,
// optional depth write, optional ALPHA BLEND) + framebuffer write. All-integer arithmetic
// identical to the golden model in sim/gfx/gfx.hpp, so the two agree pixel-for-pixel.
//
//  * Perspective correction: each vertex carries q = 1/w in Q16; attributes interpolate as
//    attr = sum(wi*ai*qi) / sum(wi*qi). Equal qi reduces EXACTLY to the affine
//    sum(wi*ai)/area (identity w0+w1+w2 = area), so M9/M10 scenes are unchanged.
//    Depth stays screen-linear (standard Z-buffer).
//  * Texture memory holds a mip PYRAMID (level 0 first, each level half-size, square
//    power-of-two). The per-triangle LOD comes from the texel-area/pixel-area ratio
//    (each level covers 4x the area). Bilinear = 4 texels, Q8 weights, >>16 blend,
//    half-texel center offset.
//  * ROP: depth LESS test; alpha blend C = (Cs*a + Cd*(255-a)) / 255; depth_write can be
//    disabled for translucent passes.
//
// One candidate pixel per cycle over the bounding box; divides modeled single-cycle
// (a real build would pipeline them; correctness first).
//
// Conventions: ANSI ports, in-header import, synchronous active-high reset.

`default_nettype none

module raster
  import sirion_pkg::*;
#(
  parameter int W = 220,
  parameter int H = 220,
  parameter int TEX_W = 64,          // square power-of-two
  parameter int TEX_H = 64,
  parameter int FRAG_MAX = 16384     // fragment-buffer capacity (M19 emit mode)
) (
  input  wire        clk,
  input  wire        rst,
  // clear the framebuffer (color := clear_color, depth := far)
  input  wire        clear,
  input  wire [23:0] clear_color,
  // triangle submission (screen-space integer vertices; q = 1/w in Q16)
  input  wire        tri_valid,
  input  wire signed [31:0] x0, y0, z0, x1, y1, z1, x2, y2, z2,
  input  wire [7:0]  r0, g0, b0, r1, g1, b1, r2, g2, b2,
  input  wire signed [31:0] q0, q1, q2,
  input  wire        cull_backface,
  // texturing: per-vertex UV (8.8 texel-space), enable + filtering/mip modes, load port
  input  wire        textured,
  input  wire        tex_bilinear,
  input  wire        tex_mip,
  input  wire signed [31:0] u0, v0, u1, v1, u2, v2,
  input  wire        tex_we,
  input  wire [$clog2(TEX_W*TEX_H*2)-1:0] tex_waddr,   // pyramid-sized
  input  wire [23:0] tex_wdata,
  // ROP state
  input  wire        blend_en,
  input  wire [7:0]  alpha,
  input  wire        depth_write,
  // ---- M19 programmable-shading hooks ----
  // EMIT mode: covered pixels write fragment records (pidx/z, uv, rgb varyings) to the
  // fragment buffer instead of shading (depth test deferred to the ROP pass).
  input  wire        emit_mode,
  input  wire        frag_rst,       // zero frag_count (new draw) without clearing the FB
  output logic [31:0] frag_count,
  input  wire [$clog2(FRAG_MAX)-1:0] fdbg_idx,     // host fragment readback
  output logic [31:0] fdbg_w0,                     // {z[15:0], pidx[15:0]}
  output logic [31:0] fdbg_w1,                     // {v[15:0], u[15:0]}
  output logic [31:0] fdbg_w2,                     // {8'd0, r, g, b}
  // ROP input: one externally-shaded fragment per cycle (depth test + blend + write,
  // using the blend_en/alpha/depth_write inputs directly)
  input  wire        fin_valid,
  input  wire [$clog2(W*H)-1:0] fin_pidx,
  input  wire signed [31:0]     fin_z,
  input  wire [23:0]            fin_color,
  output logic       busy,
  // color-buffer readback (async): pixel index = y*W + x
  input  wire [$clog2(W*H)-1:0] fb_raddr,
  output logic [23:0]           fb_rcolor,
  // performance counters (cleared with the framebuffer)
  output logic [31:0]           perf_pixels,   // candidate pixels tested
  output logic [31:0]           perf_frags     // fragments that passed the depth test
);

  localparam int NPX   = W * H;
  localparam int IDXW  = $clog2(NPX);
  localparam logic signed [31:0] FAR = 32'sh7FFFFFFF;
  localparam int MAXLVL = $clog2(TEX_W) + 1;           // pyramid depth for a square pow-2

  // pyramid size + level offsets (constant function evaluation)
  function automatic int f_pyr_size();
    int s;
    s = 0;
    for (int l = 0; l < MAXLVL; l++) s += (TEX_W >> l) * (TEX_H >> l);
    f_pyr_size = s;
  endfunction
  localparam int TPYR  = f_pyr_size();
  localparam int TIDXW = $clog2(TEX_W*TEX_H*2);

  function automatic int f_lvl_off(input int l);
    int o;
    o = 0;
    for (int i = 0; i < MAXLVL; i++) if (i < l) o += (TEX_W >> i) * (TEX_H >> i);
    f_lvl_off = o;
  endfunction

  logic [23:0]        colbuf [NPX];
  logic signed [31:0] depbuf [NPX];
  assign fb_rcolor = colbuf[fb_raddr];

  // ---- fragment buffer (M19 emit mode): three parallel word planes per record ----
  logic [31:0] fragw0 [FRAG_MAX];
  logic [31:0] fragw1 [FRAG_MAX];
  logic [31:0] fragw2 [FRAG_MAX];
  assign fdbg_w0 = fragw0[fdbg_idx];
  assign fdbg_w1 = fragw1[fdbg_idx];
  assign fdbg_w2 = fragw2[fdbg_idx];
  logic emit_on;

  logic [23:0] texmem [TPYR];
  always_ff @(posedge clk) if (tex_we) texmem[tex_waddr] <= tex_wdata;

  // ---- latched triangle + draw state ----
  logic signed [31:0] X0,Y0,Z0, X1,Y1,Z1, X2,Y2,Z2;
  logic signed [31:0] R0,G0,B0, R1,G1,B1, R2,G2,B2;
  logic signed [31:0] U0,V0, U1,V1, U2,V2;
  logic signed [31:0] Q0,Q1,Q2;
  logic               tex_on, bil_on, blnd_on, dwrite_on;
  logic [7:0]         alpha_q;
  logic [3:0]         lod;
  logic signed [63:0] area;
  logic               area_pos, do_cull;
  logic signed [31:0] minx, maxx, miny, maxy, px, py;

  typedef enum logic [1:0] {S_IDLE, S_CLEAR, S_SCAN} state_e;
  state_e state;
  logic [IDXW:0] cidx;   // clear index

  function automatic logic signed [63:0] edgef
      (input logic signed [31:0] ax, ay, bx, by, cx, cy);
    logic signed [63:0] dbx, dby, dpx, dpy;
    dbx = bx - ax; dby = by - ay; dpx = cx - ax; dpy = cy - ay;
    edgef = dbx * dpy - dby * dpx;
  endfunction

  function automatic logic signed [31:0] mn3(input logic signed [31:0] a,b,c);
    logic signed [31:0] m; m = (a < b) ? a : b; mn3 = (m < c) ? m : c;
  endfunction
  function automatic logic signed [31:0] mx3(input logic signed [31:0] a,b,c);
    logic signed [31:0] m; m = (a > b) ? a : b; mx3 = (m > c) ? m : c;
  endfunction
  function automatic logic signed [31:0] clamp(input logic signed [31:0] v, lo, hi);
    clamp = (v < lo) ? lo : (v > hi) ? hi : v;
  endfunction

  // per-triangle mip level from the texel-area / pixel-area ratio (mirrors the golden)
  function automatic logic [3:0] f_lod(
      input logic signed [31:0] uu0, vv0, uu1, vv1, uu2, vv2,
      input logic signed [63:0] parea);
    logic signed [63:0] du1, dv1, du2, dv2, cx;
    logic [63:0] ta, pa, ratio;
    logic [3:0] l;
    begin
      du1 = uu1 - uu0; dv1 = vv1 - vv0; du2 = uu2 - uu0; dv2 = vv2 - vv0;
      cx  = du1 * dv2 - du2 * dv1;
      ta  = cx < 0 ? 64'(-cx) : 64'(cx);
      pa  = (parea < 0 ? 64'(-parea) : 64'(parea)) << 16;
      ratio = (pa != 0) ? ta / pa : 64'd0;
      l = 4'd0;
      for (int i = 0; i < MAXLVL; i++)
        if ((int'(l) + 1 < MAXLVL) && ((ratio >> (2 * (int'(l) + 1))) != 0)) l = l + 4'd1;
      f_lod = l;
    end
  endfunction

  // ---- per-pixel coverage + interpolation (combinational) ----
  logic signed [63:0] w0, w1, w2;
  logic               covered;
  logic signed [63:0] zi, pw;
  logic [IDXW-1:0]    pidx;
  assign w0 = edgef(X1,Y1, X2,Y2, px,py);
  assign w1 = edgef(X2,Y2, X0,Y0, px,py);
  assign w2 = edgef(X0,Y0, X1,Y1, px,py);
  assign covered = area_pos ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                           : (w0 <= 0 && w1 <= 0 && w2 <= 0);
  assign zi = (area != 0) ? (w0*Z0 + w1*Z1 + w2*Z2) / area : 64'sd0;
  assign pw = w0*Q0 + w1*Q1 + w2*Q2;
  assign pidx = py[IDXW-1:0] * W + px[IDXW-1:0];

  // perspective-correct attribute interpolation: sum(wi*ai*qi) / sum(wi*qi)
  function automatic logic signed [31:0] pinterp(
      input logic signed [31:0] a0, a1, a2);
    logic signed [63:0] num;
    begin
      num = w0*a0*Q0 + w1*a1*Q1 + w2*a2*Q2;
      pinterp = (pw != 0) ? 32'(num / pw) : 32'sd0;
    end
  endfunction

  wire signed [31:0] ri = pinterp(R0, R1, R2);
  wire signed [31:0] gi = pinterp(G0, G1, G2);
  wire signed [31:0] bi = pinterp(B0, B1, B2);
  wire signed [31:0] ui = pinterp(U0, U1, U2);
  wire signed [31:0] vi = pinterp(V0, V1, V2);

  // ---- texture sampling (level `lod`; nearest or bilinear) ----
  wire [3:0] L = tex_mip ? lod : 4'd0;
  wire signed [31:0] lw = 32'(TEX_W) >>> L;
  wire signed [31:0] lh = 32'(TEX_H) >>> L;
  // combinational level offset (small mux over MAXLVL constants)
  logic [31:0] loff;
  always_comb begin
    loff = 0;
    for (int i = 0; i < MAXLVL; i++) if (int'(L) == i) loff = 32'(f_lvl_off(i));
  end

  function automatic logic [23:0] texel(input logic signed [31:0] tx, ty);
    logic signed [31:0] cx, cy;
    begin
      cx = clamp(tx, 0, lw-1);
      cy = clamp(ty, 0, lh-1);
      texel = texmem[TIDXW'(loff + 32'(cy) * 32'(lw) + 32'(cx))];
    end
  endfunction

  // nearest sample at level L
  wire [23:0] tex_nearest = texel(ui >>> (8 + int'(L)), vi >>> (8 + int'(L)));

  // bilinear: level-scaled coords, half-texel center offset, Q8 weights
  wire signed [31:0] uu = (ui >>> int'(L)) - 32'sd128;
  wire signed [31:0] vv = (vi >>> int'(L)) - 32'sd128;
  wire signed [31:0] btx = uu >>> 8, bty = vv >>> 8;
  wire [7:0] fx = uu[7:0], fy = vv[7:0];
  wire [23:0] t00 = texel(btx,       bty);
  wire [23:0] t10 = texel(btx+32'sd1, bty);
  wire [23:0] t01 = texel(btx,       bty+32'sd1);
  wire [23:0] t11 = texel(btx+32'sd1, bty+32'sd1);

  function automatic logic [7:0] bilerp(input logic [7:0] c00, c10, c01, c11);
    logic [31:0] acc;
    begin
      acc = 32'(c00) * (32'd256 - 32'(fx)) * (32'd256 - 32'(fy))
          + 32'(c10) * 32'(fx)             * (32'd256 - 32'(fy))
          + 32'(c01) * (32'd256 - 32'(fx)) * 32'(fy)
          + 32'(c11) * 32'(fx)             * 32'(fy);
      bilerp = 8'(acc >> 16);
    end
  endfunction

  wire [23:0] tex_bilin = {bilerp(t00[23:16], t10[23:16], t01[23:16], t11[23:16]),
                           bilerp(t00[15:8],  t10[15:8],  t01[15:8],  t11[15:8]),
                           bilerp(t00[7:0],   t10[7:0],   t01[7:0],   t11[7:0])};

  wire [23:0] src_color = tex_on ? (bil_on ? tex_bilin : tex_nearest)
                                 : {ri[7:0], gi[7:0], bi[7:0]};

  // ---- ROP: depth test + alpha blend (M18) ----
  wire signed [31:0] zi_s = zi[31:0];
  wire pass_depth = covered && (zi_s < depbuf[pidx]);

  function automatic logic [7:0] blend1(input logic [7:0] s, d);
    blend1 = 8'((32'(s) * 32'(alpha_q) + 32'(d) * (32'd255 - 32'(alpha_q))) / 32'd255);
  endfunction
  wire [23:0] dstc = colbuf[pidx];
  wire [23:0] out_color = blnd_on ? {blend1(src_color[23:16], dstc[23:16]),
                                     blend1(src_color[15:8],  dstc[15:8]),
                                     blend1(src_color[7:0],   dstc[7:0])}
                                  : src_color;

  assign busy = (state != S_IDLE);

  always_ff @(posedge clk) begin
    if (rst) begin
      state <= S_IDLE; cidx <= '0; perf_pixels <= '0; perf_frags <= '0;
      frag_count <= '0; emit_on <= 1'b0;
    end else begin
      unique case (state)
        S_IDLE: begin
          if (clear) begin
            cidx <= '0; state <= S_CLEAR; perf_pixels <= '0; perf_frags <= '0;
            frag_count <= '0;
          end else if (tri_valid) begin
            X0<=x0;Y0<=y0;Z0<=z0; X1<=x1;Y1<=y1;Z1<=z1; X2<=x2;Y2<=y2;Z2<=z2;
            R0<={24'd0,r0};G0<={24'd0,g0};B0<={24'd0,b0};
            R1<={24'd0,r1};G1<={24'd0,g1};B1<={24'd0,b1};
            R2<={24'd0,r2};G2<={24'd0,g2};B2<={24'd0,b2};
            U0<=u0;V0<=v0; U1<=u1;V1<=v1; U2<=u2;V2<=v2;
            Q0<=q0; Q1<=q1; Q2<=q2;
            tex_on<=textured; bil_on<=tex_bilinear;
            blnd_on<=blend_en; alpha_q<=alpha; dwrite_on<=depth_write;
            emit_on<=emit_mode;
            lod <= tex_mip ? f_lod(u0,v0, u1,v1, u2,v2, edgef(x0,y0, x1,y1, x2,y2)) : 4'd0;
            area     <= edgef(x0,y0, x1,y1, x2,y2);
            area_pos <= edgef(x0,y0, x1,y1, x2,y2) > 0;
            do_cull  <= cull_backface;
            minx <= clamp(mn3(x0,x1,x2), 0, W-1);
            maxx <= clamp(mx3(x0,x1,x2), 0, W-1);
            miny <= clamp(mn3(y0,y1,y2), 0, H-1);
            maxy <= clamp(mx3(y0,y1,y2), 0, H-1);
            px   <= clamp(mn3(x0,x1,x2), 0, W-1);
            py   <= clamp(mn3(y0,y1,y2), 0, H-1);
            state <= S_SCAN;
          end
        end

        S_CLEAR: begin
          colbuf[cidx[IDXW-1:0]] <= clear_color;
          depbuf[cidx[IDXW-1:0]] <= FAR;
          if (cidx == NPX-1) state <= S_IDLE;
          else cidx <= cidx + 1'b1;
        end

        S_SCAN: begin
          if (area == 0 || (do_cull && !area_pos)) begin
            state <= S_IDLE;
          end else begin
            perf_pixels <= perf_pixels + 1'b1;
            if (emit_on) begin
              // M19: emit a fragment record for every covered pixel (late-Z at the ROP)
              if (covered && pw != 0 && frag_count < FRAG_MAX) begin
                fragw0[frag_count[$clog2(FRAG_MAX)-1:0]] <= {zi_s[15:0], 16'(pidx)};
                fragw1[frag_count[$clog2(FRAG_MAX)-1:0]] <= {vi[15:0], ui[15:0]};
                fragw2[frag_count[$clog2(FRAG_MAX)-1:0]] <= {8'd0, ri[7:0], gi[7:0], bi[7:0]};
                frag_count <= frag_count + 1'b1;
                perf_frags <= perf_frags + 1'b1;
              end
            end else if (pass_depth && pw != 0) begin
              colbuf[pidx] <= out_color;
              if (dwrite_on) depbuf[pidx] <= zi_s;
              perf_frags <= perf_frags + 1'b1;
            end
            if (px == maxx) begin
              px <= minx;
              if (py == maxy) state <= S_IDLE;
              else py <= py + 1'b1;
            end else begin
              px <= px + 1'b1;
            end
          end
        end
      endcase

      if (frag_rst) frag_count <= '0;   // new draw call (M20 sequencer)

      // ---- M19 ROP input: one externally-shaded fragment per cycle (any state) ----
      if (fin_valid) begin
        if (fin_z < depbuf[fin_pidx]) begin
          colbuf[fin_pidx] <= blend_en
            ? {8'((32'(fin_color[23:16]) * 32'(alpha) + 32'(colbuf[fin_pidx][23:16]) * (32'd255 - 32'(alpha))) / 32'd255),
               8'((32'(fin_color[15:8])  * 32'(alpha) + 32'(colbuf[fin_pidx][15:8])  * (32'd255 - 32'(alpha))) / 32'd255),
               8'((32'(fin_color[7:0])   * 32'(alpha) + 32'(colbuf[fin_pidx][7:0])   * (32'd255 - 32'(alpha))) / 32'd255)}
            : fin_color;
          if (depth_write) depbuf[fin_pidx] <= fin_z;
        end
      end
    end
  end

`ifndef SYNTHESIS
  a_idx_range: assert property (@(posedge clk) disable iff (rst)
    (state == S_SCAN) |-> (pidx < NPX));
`endif

endmodule

`default_nettype wire
