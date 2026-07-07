// fp_alu.sv — single-precision floating-point ALU (M8): FADD, FMUL, FMA.
//
// Combinational IEEE-754 binary32 add/multiply with round-toward-zero (truncation) and
// flush-to-zero denormals. Inf/NaN are out of scope (the testbench uses finite normals).
// A companion C++ golden (fp_ref, in the testbench) implements the identical integer
// algorithm so the two agree bit-for-bit; directed checks pin exactly-representable results.
//
// op: 0 = FADD, 1 = FMUL, 2 = FMA (a*b + c via one multiply then one add),
//     3 = FMIN, 4 = FMAX (total-order on FTZ-canonicalized keys),
//     5 = I2F (int32 -> float, truncate), 6 = F2I (float -> int32, truncate, saturate).
// All mirror the fp_*_ref golden functions in sim/iss/isa.hpp bit-for-bit.
//
// Conventions: ANSI ports, in-header import, purely combinational.

`default_nettype none

module fp_alu
  import sirion_pkg::*;
(
  input  wire  [2:0] op,
  input  wire  word_t a,
  input  wire  word_t b,
  input  wire  word_t c,      // addend for FMA
  output word_t      y
);

  // ---- FMUL ----
  function automatic word_t f_mul(input word_t x, input word_t z);
    logic        sx, sz, s;
    logic [7:0]  ex, ez;
    logic [22:0] mx, mz;
    logic [23:0] fx, fz;
    logic [47:0] prod;
    logic [9:0]  e;          // extended exponent (signed-ish)
    logic [22:0] mant;
    begin
      sx = x[31]; ex = x[30:23]; mx = x[22:0];
      sz = z[31]; ez = z[30:23]; mz = z[22:0];
      s  = sx ^ sz;
      if (ex == 8'd0 || ez == 8'd0) begin
        f_mul = {s, 31'd0};                       // flush: a*0 = 0
      end else begin
        fx = {1'b1, mx}; fz = {1'b1, mz};
        prod = fx * fz;                            // [1,4) * 2^46
        e = {2'b0, ex} + {2'b0, ez} - 10'd127;
        if (prod[47]) begin mant = prod[46:24]; e = e + 10'd1; end
        else               mant = prod[45:23];
        f_mul = {s, e[7:0], mant};
      end
    end
  endfunction

  // ---- FADD ----
  function automatic word_t f_add(input word_t x, input word_t z);
    logic        s;
    logic [7:0]  ex, ez, e;
    logic [23:0] fx, fz, big, aligned, sub;
    logic [24:0] s25;
    logic [4:0]  diff;
    logic        swap;
    int          i;
    begin
      if (x[30:23] == 8'd0) f_add = z;             // x == 0
      else if (z[30:23] == 8'd0) f_add = x;        // z == 0
      else begin
        // order so the larger-magnitude operand is (ex, fx, sign s)
        swap = (z[30:23] > x[30:23]) ||
               (z[30:23] == x[30:23] && z[22:0] > x[22:0]);
        ex = swap ? z[30:23] : x[30:23];
        ez = swap ? x[30:23] : z[30:23];
        s  = swap ? z[31]    : x[31];
        fx = {1'b1, (swap ? z[22:0] : x[22:0])};
        fz = {1'b1, (swap ? x[22:0] : z[22:0])};
        big = fx; e = ex;
        diff = (ex - ez > 8'd24) ? 5'd24 : 5'(ex - ez);
        aligned = fz >> diff;                       // round-toward-zero alignment
        if (x[31] == z[31]) begin                   // like signs: add
          s25 = {1'b0, big} + {1'b0, aligned};
          if (s25[24]) begin e = e + 8'd1; f_add = {s, e, s25[23:1]}; end
          else                              f_add = {s, e, s25[22:0]};
        end else begin                              // unlike signs: subtract
          sub = big - aligned;
          if (sub == 24'd0) f_add = 32'd0;
          else begin
            for (i = 0; i < 24; i++)
              if (!sub[23]) begin sub = sub << 1; e = e - 8'd1; end
            f_add = {s, e, sub[22:0]};
          end
        end
      end
    end
  endfunction

  word_t prod_fma;
  assign prod_fma = f_mul(a, b);

  // total-order key (mirrors fp_key in isa.hpp): FTZ-canonicalize +-0, then map so an
  // unsigned compare gives float ordering.
  function automatic word_t f_key(input word_t x);
    word_t v;
    begin
      v = (x[30:23] == 8'd0) ? 32'd0 : x;
      f_key = v[31] ? ~v : (v | 32'h8000_0000);
    end
  endfunction

  // int32 -> float, truncate (mirrors fp_i2f_ref)
  function automatic word_t f_i2f(input word_t x);
    logic        s;
    logic [31:0] mag;
    logic [4:0]  p;
    logic [22:0] mant;
    begin
      if (x == 32'd0) f_i2f = 32'd0;
      else begin
        s   = x[31];
        mag = s ? (~x + 32'd1) : x;
        p   = 5'd31;
        for (int i = 30; i >= 0; i--)
          if (!mag[p]) p = p - 5'd1;                 // find msb (mag != 0)
        mant = (p > 5'd23) ? 23'((mag >> (p - 5'd23))) : 23'((mag << (5'd23 - p)));
        f_i2f = {s, 8'(8'd127 + {3'b0, p}), mant};
      end
    end
  endfunction

  // float -> int32, truncate toward zero, saturate (mirrors fp_f2i_ref)
  function automatic word_t f_f2i(input word_t x);
    logic        s;
    logic [7:0]  ex;
    logic signed [8:0] e;
    logic [31:0] f, mag;
    begin
      s = x[31]; ex = x[30:23];
      if (ex == 8'd0) f_f2i = 32'd0;                 // FTZ
      else begin
        e = {1'b0, ex} - 9'd127;
        if (e < 0)        f_f2i = 32'd0;
        else if (e > 30)  f_f2i = s ? 32'h8000_0000 : 32'h7FFF_FFFF;   // saturate
        else begin
          f   = {9'd1, x[22:0]};                     // (1 << 23) | mant
          mag = (e <= 23) ? (f >> (9'd23 - e)) : (f << (e - 9'd23));
          f_f2i = s ? (~mag + 32'd1) : mag;
        end
      end
    end
  endfunction

  always_comb begin
    unique case (op)
      3'd0:    y = f_add(a, b);
      3'd1:    y = f_mul(a, b);
      3'd2:    y = f_add(prod_fma, c);
      3'd3:    y = (f_key(a) < f_key(b)) ? a : b;    // FMIN
      3'd4:    y = (f_key(a) > f_key(b)) ? a : b;    // FMAX
      3'd5:    y = f_i2f(a);
      3'd6:    y = f_f2i(a);
      default: y = '0;
    endcase
  end

endmodule

`default_nettype wire
