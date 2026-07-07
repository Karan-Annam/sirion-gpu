// test_sfu.cpp — self-checking testbench for rtl/compute/sfu.sv (M16).
//
// (1) RTL == sfu_*_ref bit-exact over random values (the same functions the ISS runs);
// (2) accuracy: RCP/RSQRT within ~2 ULP of libm; SIN/COS within 2e-4 absolute.
#include "Vsfu.h"
#include "../rtl_driver.hpp"
#include "../iss/isa.hpp"
#include "test_framework.hpp"
#include <verilated.h>
#include <cmath>
#include <cstring>
#include <random>

using namespace sirion;

static uint32_t f2u(float f){ uint32_t u; std::memcpy(&u,&f,4); return u; }
static float    u2f(uint32_t u){ float f; std::memcpy(&f,&u,4); return f; }

static Vsfu* D;
static uint32_t rtl(int fn, uint32_t a){ D->func=fn; D->a=a; D->eval(); return D->y; }

TEST(sfu_vs_golden) {                       // bit-exact vs the shared reference
  VerilatedContext ctx; D = new Vsfu(&ctx);
  std::mt19937 rng(0x5F3759DF);
  auto rnorm = [&]{ uint32_t s=rng()&1, e=90+rng()%70, m=rng()&0x7FFFFF; return (s<<31)|(e<<23)|m; };
  for (int i = 0; i < 20000; ++i) {
    uint32_t a = rnorm();
    CHECK_EQ(rtl(MUFU_RCP,   a), sfu_rcp_ref(a));
    CHECK_EQ(rtl(MUFU_RSQRT, a), sfu_rsqrt_ref(a));
    CHECK_EQ(rtl(MUFU_SIN,   a), sfu_sin_ref(a));
    CHECK_EQ(rtl(MUFU_COS,   a), sfu_cos_ref(a));
  }
  delete D;
}

TEST(sfu_accuracy) {                        // vs libm within the documented bounds
  VerilatedContext ctx; D = new Vsfu(&ctx);
  std::mt19937 rng(0xACC);
  int checked = 0;
  for (int i = 0; i < 5000; ++i) {
    float x = (float)(rng() % 100000) / 1000.0f + 0.001f;    // (0, 100]
    // RCP: relative error <= ~1 ULP
    float rc = u2f(rtl(MUFU_RCP, f2u(x)));
    CHECK(std::fabs(rc - 1.0f/x) <= 2.4e-7f * (1.0f/x) + 1e-30f);
    // RSQRT: <= ~2 ULP
    float rs = u2f(rtl(MUFU_RSQRT, f2u(x)));
    CHECK(std::fabs(rs - 1.0f/std::sqrt(x)) <= 5e-7f * (1.0f/std::sqrt(x)) + 1e-30f);
    // SIN/COS over +-8 rad: absolute error <= 2e-4
    float a = ((float)(rng() % 16000) / 1000.0f) - 8.0f;
    CHECK(std::fabs(u2f(rtl(MUFU_SIN, f2u(a))) - std::sin(a)) < 2e-4f);
    CHECK(std::fabs(u2f(rtl(MUFU_COS, f2u(a))) - std::cos(a)) < 2e-4f);
    ++checked;
  }
  CHECK_EQ(checked, 5000);
  delete D;
}

int main(int, char**) { return tf::run_all(); }
