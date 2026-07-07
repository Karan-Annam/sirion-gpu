// sfu.sv — special-function unit (M16): RCP, RSQRT, SIN, COS on binary32.
//
// Approximate transcendental unit, one result per call (combinational, one per lane):
//   RCP   — exact truncated reciprocal: 2^46 / (1.m) integer division      (<= 1 ULP)
//   RSQRT — truncated integer square root then RCP                          (<= ~2 ULP)
//   SIN/COS — quarter-wave 65-entry Q15 LUT + linear interpolation over a
//             Q16 fraction-of-turn phase (~1e-4 absolute; range in ISA.md)
// Bit-exactly mirrors sfu_*_ref in sim/iss/isa.hpp (the ISS executes those).
// Same float conventions as fp_alu: round-toward-zero, FTZ, Inf/NaN out of scope
// (the Inf bit pattern is produced for rcp(0)/rsqrt(0) but never consumed specially).
//
// Conventions: ANSI ports, in-header import, purely combinational.

`default_nettype none

module sfu
  import sirion_pkg::*;
(
  input  wire  [2:0] func,    // MUFU_RCP / MUFU_RSQRT / MUFU_SIN / MUFU_COS
  input  wire  word_t a,
  output word_t      y
);

  // ---- RCP (mirrors sfu_rcp_ref) ----
  function automatic word_t f_rcp(input word_t x);
    logic        s;
    logic [7:0]  e;
    logic [22:0] m;
    logic [63:0] f, r;
    begin
      s = x[31]; e = x[30:23]; m = x[22:0];
      if (e == 8'd0)        f_rcp = {s, 8'hFF, 23'd0};          // 1/0 -> Inf pattern
      else if (e >= 8'd253) f_rcp = {s, 31'd0};                 // underflow -> signed 0
      else if (m == 23'd0)  f_rcp = {s, 8'(8'd254 - e), 23'd0}; // exact power of two
      else begin
        f = {40'd0, 1'b1, m};
        r = (64'd1 << 46) / f;                                  // in [2^22, 2^23)
        f_rcp = {s, 8'(8'd253 - e), 23'((r << 1))};
      end
    end
  endfunction

  // ---- integer sqrt of a 48-bit value (mirrors sfu_isqrt48) ----
  function automatic logic [63:0] f_isqrt48(input logic [63:0] v_in);
    logic [63:0] v, r, b;
    begin
      v = v_in; r = 64'd0; b = 64'd1 << 46;
      for (int i = 0; i < 24; i++) begin
        if (v >= r + b) begin v = v - (r + b); r = (r >> 1) + b; end
        else r = r >> 1;
        b = b >> 2;
      end
      f_isqrt48 = r;
    end
  endfunction

  // ---- sqrt of a positive float (mirrors sfu_sqrt_ref) ----
  function automatic word_t f_sqrt(input word_t x);
    logic [7:0]         e;
    logic [22:0]        m;
    logic signed [9:0]  bigE;
    logic [63:0]        g, q;
    begin
      e = x[30:23]; m = x[22:0];
      if (e == 8'd0) f_sqrt = 32'd0;
      else begin
        bigE = {2'b0, e} - 10'sd127;
        g = {40'd0, 1'b1, m};
        if (bigE[0]) begin g = g << 1; bigE = bigE - 10'sd1; end
        q = f_isqrt48(g << 23);                                 // in [2^23, 2^24)
        f_sqrt = (32'(127 + (bigE >>> 1)) << 23) | {9'd0, q[22:0]};
      end
    end
  endfunction

  function automatic word_t f_rsqrt(input word_t x);
    begin
      if (x[30:23] == 8'd0) f_rsqrt = 32'h7F80_0000;            // rsqrt(0) -> Inf pattern
      else if (x[31])       f_rsqrt = 32'd0;                    // out of domain -> 0
      else                  f_rsqrt = f_rcp(f_sqrt(x));
    end
  endfunction

  // ---- SIN/COS: quarter-wave Q15 LUT (mirrors SFU_SIN_LUT / sfu_*_ref) ----
  localparam logic [16:0] SIN_LUT [65] = '{
    17'd0, 17'd804, 17'd1608, 17'd2411, 17'd3212, 17'd4011, 17'd4808, 17'd5602,
    17'd6393, 17'd7180, 17'd7962, 17'd8740, 17'd9512, 17'd10279, 17'd11039, 17'd11793,
    17'd12540, 17'd13279, 17'd14010, 17'd14733, 17'd15447, 17'd16151, 17'd16846, 17'd17531,
    17'd18205, 17'd18868, 17'd19520, 17'd20160, 17'd20788, 17'd21403, 17'd22006, 17'd22595,
    17'd23170, 17'd23732, 17'd24279, 17'd24812, 17'd25330, 17'd25833, 17'd26320, 17'd26791,
    17'd27246, 17'd27684, 17'd28106, 17'd28511, 17'd28899, 17'd29269, 17'd29622, 17'd29957,
    17'd30274, 17'd30572, 17'd30853, 17'd31114, 17'd31357, 17'd31581, 17'd31786, 17'd31972,
    17'd32138, 17'd32286, 17'd32413, 17'd32522, 17'd32610, 17'd32679, 17'd32729, 17'd32758,
    17'd32768
  };

  // x * (1/2pi), RTZ multiply — small copy of fp_alu's f_mul (kept local so the SFU is
  // self-contained; constant b = 0x3E22F983).
  function automatic word_t f_mul2pi(input word_t x);
    logic        sx, s;
    logic [7:0]  ex;
    logic [22:0] mx;
    logic [47:0] prod;
    logic [9:0]  e;
    logic [22:0] mant;
    begin
      sx = x[31]; ex = x[30:23]; mx = x[22:0];
      if (ex == 8'd0) f_mul2pi = {x[31], 31'd0};
      else begin
        s = sx;                                                  // 1/2pi is positive
        prod = ({1'b1, mx}) * (24'h22F983 | 24'h800000);         // {1,m} * {1, 0x22F983}
        e = {2'b0, ex} + 10'd124 - 10'd127;                      // exp(1/2pi) = 124
        if (prod[47]) begin mant = prod[46:24]; e = e + 10'd1; end
        else               mant = prod[45:23];
        f_mul2pi = {s, e[7:0], mant};
      end
    end
  endfunction

  // fraction-of-turn, Q16 (mirrors sfu_phase16)
  function automatic logic [15:0] f_phase16(input word_t x);
    word_t       t;
    logic [7:0]  e;
    logic [22:0] m;
    logic        s;
    integer      sh;
    logic [63:0] fx;
    logic [15:0] p;
    begin
      t = f_mul2pi(x);
      e = t[30:23]; m = t[22:0]; s = t[31];
      if (e == 8'd0) f_phase16 = 16'd0;
      else begin
        sh = int'(e) - 127 - 7;
        fx = {40'd0, 1'b1, m};
        if (sh > 24)       p = 16'd0;
        else if (sh >= 0)  p = 16'((fx << sh));
        else if (sh > -24) p = 16'((fx >> (-sh)));
        else               p = 16'd0;
        f_phase16 = s ? (16'd0 - p) : p;                         // mod-2^16 negate
      end
    end
  endfunction

  // |sin| in Q15 with sign in bit 31 (mirrors sfu_sinq15)
  function automatic word_t f_sinq15(input logic [15:0] phase16);
    logic [1:0]  quad;
    logic [14:0] idx14;
    logic [16:0] v, a0, a1;
    logic [5:0]  i;
    logic [7:0]  f8;
    logic        mirrored_top;
    begin
      quad  = phase16[15:14];
      // mirror in quadrants 1/3: idx = 0x4000 - idx (result fits 15 bits)
      idx14 = (quad[0]) ? (15'h4000 - {1'b0, phase16[13:0]}) : {1'b0, phase16[13:0]};
      if (idx14 >= 15'h4000) v = SIN_LUT[64];
      else begin
        i  = idx14[13:8];
        f8 = idx14[7:0];
        a0 = SIN_LUT[{1'b0, i}];
        a1 = SIN_LUT[{1'b0, i} + 7'd1];    // 7-bit index: i=63 must reach LUT[64], not wrap
        v  = a0 + 17'((({1'b0, a1 - a0}) * {10'd0, f8}) >> 8);
      end
      f_sinq15 = {quad[1], 14'd0, v};
    end
  endfunction

  // Q15 magnitude+sign -> float (mirrors sfu_q15_to_f)
  function automatic word_t f_q15_to_f(input word_t vq);
    logic        s;
    logic [30:0] v;
    logic [4:0]  p;
    logic [7:0]  e;
    logic [22:0] mant;
    begin
      s = vq[31]; v = vq[30:0];
      if (v == 31'd0) f_q15_to_f = 32'd0;
      else begin
        p = 5'd15;
        for (int i = 14; i >= 0; i--)
          if (!v[p] && p > 0) p = p - 5'd1;
        e = 8'(8'd127 + {3'd0, p} - 8'd15);
        mant = 23'((v << (5'd23 - p)));
        f_q15_to_f = {s, e, mant};
      end
    end
  endfunction

  always_comb begin
    unique case (func)
      MUFU_RCP:   y = f_rcp(a);
      MUFU_RSQRT: y = f_rsqrt(a);
      MUFU_SIN:   y = f_q15_to_f(f_sinq15(f_phase16(a)));
      MUFU_COS:   y = f_q15_to_f(f_sinq15(f_phase16(a) + 16'h4000));
      default:    y = '0;
    endcase
  end

endmodule

`default_nettype wire
