// test_fp_alu.cpp — self-checking testbench for rtl/compute/fp_alu.sv (M8).
//
// Two kinds of checks: (1) directed known-answer FP results on exactly-representable values
// (these must equal what real IEEE arithmetic gives), and (2) a C++ golden that mirrors the
// RTL's exact integer algorithm, checked bit-for-bit over random finite normals.
#include "Vfp_alu.h"
#include "../rtl_driver.hpp"   // sc_time_stamp stub
#include "../iss/isa.hpp"      // shared fp_*_ref golden (M15)
#include "test_framework.hpp"
#include <verilated.h>
#include <cstring>
#include <random>

static uint32_t f2u(float f){ uint32_t u; std::memcpy(&u,&f,4); return u; }
static float    u2f(uint32_t u){ float f; std::memcpy(&f,&u,4); return f; }

// ---- golden model: identical integer algorithm to fp_alu.sv (round-toward-zero) ----
static uint32_t ref_mul(uint32_t x, uint32_t z) {
  int sx=x>>31, sz=z>>31, s=sx^sz;
  int ex=(x>>23)&0xFF, ez=(z>>23)&0xFF;
  uint32_t mx=x&0x7FFFFF, mz=z&0x7FFFFF;
  if (ex==0||ez==0) return (uint32_t)s<<31;
  uint64_t fx=(1u<<23)|mx, fz=(1u<<23)|mz, prod=fx*fz;
  int e=ex+ez-127; uint32_t mant;
  if (prod & (1ull<<47)) { mant=(prod>>24)&0x7FFFFF; e+=1; }
  else                     mant=(prod>>23)&0x7FFFFF;
  return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|mant;
}
static uint32_t ref_add(uint32_t x, uint32_t z) {
  if (((x>>23)&0xFF)==0) return z;
  if (((z>>23)&0xFF)==0) return x;
  int exx=(x>>23)&0xFF, ezz=(z>>23)&0xFF;
  uint32_t mxx=x&0x7FFFFF, mzz=z&0x7FFFFF;
  bool swap = (ezz>exx)||(ezz==exx && mzz>mxx);
  int ex=swap?ezz:exx, ez=swap?exx:ezz, s=swap?(z>>31):(x>>31), e=ex;
  uint32_t fx=(1u<<23)|(swap?mzz:mxx), fz=(1u<<23)|(swap?mxx:mzz), big=fx;
  int diff=(ex-ez>24)?24:(ex-ez);
  uint32_t aligned=fz>>diff;
  if (((x>>31)&1)==((z>>31)&1)) {
    uint32_t s25=big+aligned;
    if (s25&(1u<<24)) { e+=1; return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|((s25>>1)&0x7FFFFF); }
    return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|(s25&0x7FFFFF);
  } else {
    uint32_t sub=big-aligned;
    if (sub==0) return 0;
    for (int i=0;i<24;i++) if (!(sub&(1u<<23))) { sub<<=1; e-=1; }
    return ((uint32_t)s<<31)|(((uint32_t)e&0xFF)<<23)|(sub&0x7FFFFF);
  }
}

static Vfp_alu* D;
static uint32_t rtl(int op, uint32_t a, uint32_t b=0, uint32_t c=0){
  D->op=op; D->a=a; D->b=b; D->c=c; D->eval(); return D->y;
}

TEST(fp_directed) {                       // exactly-representable => matches real IEEE
  VerilatedContext ctx; D = new Vfp_alu(&ctx);
  CHECK_EQ(rtl(0, f2u(1.0f), f2u(2.0f)), f2u(3.0f));
  CHECK_EQ(rtl(0, f2u(1.5f), f2u(2.25f)), f2u(3.75f));
  CHECK_EQ(rtl(0, f2u(10.0f), f2u(-3.0f)), f2u(7.0f));      // subtract via unlike signs
  CHECK_EQ(rtl(0, f2u(-4.0f), f2u(1.0f)), f2u(-3.0f));
  CHECK_EQ(rtl(1, f2u(2.0f), f2u(3.0f)), f2u(6.0f));
  CHECK_EQ(rtl(1, f2u(0.5f), f2u(4.0f)), f2u(2.0f));
  CHECK_EQ(rtl(1, f2u(-2.5f), f2u(2.0f)), f2u(-5.0f));
  CHECK_EQ(rtl(2, f2u(2.0f), f2u(3.0f), f2u(1.0f)), f2u(7.0f));  // FMA 2*3+1
  delete D;
}

TEST(fp_random_vs_golden) {               // RTL implements the golden algorithm exactly
  VerilatedContext ctx; D = new Vfp_alu(&ctx);
  std::mt19937 rng(0xF9A7u);
  auto rnorm = [&]{ uint32_t s=rng()&1, e=100+rng()%50, m=rng()&0x7FFFFF; return (s<<31)|(e<<23)|m; };
  for (int i=0;i<20000;++i){
    uint32_t a=rnorm(), b=rnorm();
    CHECK_EQ(rtl(1,a,b), ref_mul(a,b));
    CHECK_EQ(rtl(0,a,b), ref_add(a,b));
  }
  delete D;
}

TEST(fp_minmax_cvt) {                     // M15 ops: FMIN/FMAX/I2F/F2I vs golden + directed
  VerilatedContext ctx; D = new Vfp_alu(&ctx);
  // directed: exact conversions and orderings
  CHECK_EQ(rtl(5, 0), f2u(0.0f));
  CHECK_EQ(rtl(5, 7), f2u(7.0f));
  CHECK_EQ(rtl(5, (uint32_t)-12), f2u(-12.0f));
  CHECK_EQ(rtl(5, 123456789), sirion::fp_i2f_ref(123456789));   // truncated (inexact) case
  CHECK_EQ(rtl(6, f2u(3.75f)), 3u);                             // truncate toward zero
  CHECK_EQ(rtl(6, f2u(-3.75f)), (uint32_t)-3);
  CHECK_EQ(rtl(6, f2u(0.25f)), 0u);
  CHECK_EQ(rtl(6, f2u(3e9f)), 0x7FFFFFFFu);                     // saturate
  CHECK_EQ(rtl(6, f2u(-3e9f)), 0x80000000u);
  CHECK_EQ(rtl(3, f2u(1.0f), f2u(2.0f)), f2u(1.0f));            // FMIN
  CHECK_EQ(rtl(3, f2u(-1.0f), f2u(-2.0f)), f2u(-2.0f));
  CHECK_EQ(rtl(4, f2u(1.0f), f2u(2.0f)), f2u(2.0f));            // FMAX
  CHECK_EQ(rtl(4, f2u(-1.0f), f2u(0.5f)), f2u(0.5f));
  // random vs the shared golden (the same functions the ISS executes)
  std::mt19937 rng(0xBEEF);
  auto rnorm = [&]{ uint32_t s=rng()&1, e=100+rng()%50, m=rng()&0x7FFFFF; return (s<<31)|(e<<23)|m; };
  for (int i = 0; i < 20000; ++i) {
    uint32_t a=rnorm(), b=rnorm(), xi=rng();
    CHECK_EQ(rtl(3,a,b), sirion::fp_min_ref(a,b));
    CHECK_EQ(rtl(4,a,b), sirion::fp_max_ref(a,b));
    CHECK_EQ(rtl(5,xi),  sirion::fp_i2f_ref(xi));
    CHECK_EQ(rtl(6,a),   sirion::fp_f2i_ref(a));
  }
  delete D;
}

TEST(fp_mul_near_ieee) {                  // FMUL is within 1 ULP of hardware IEEE (round nearest)
  VerilatedContext ctx; D = new Vfp_alu(&ctx);
  std::mt19937 rng(0x1234u);
  auto rnorm = [&]{ uint32_t s=rng()&1, e=110+rng()%30, m=rng()&0x7FFFFF; return (s<<31)|(e<<23)|m; };
  for (int i=0;i<20000;++i){
    uint32_t a=rnorm(), b=rnorm();
    uint32_t got=rtl(1,a,b), want=f2u(u2f(a)*u2f(b));
    long d = (long)(got & 0x7FFFFF) - (long)(want & 0x7FFFFF);
    CHECK((got>>23)==(want>>23) && (d==0 || d==-1));   // truncation is 0 or -1 ULP mantissa
  }
  delete D;
}

int main(int, char**) { return tf::run_all(); }
