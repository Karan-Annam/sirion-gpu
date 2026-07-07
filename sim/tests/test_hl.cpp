// test_hl.cpp — end-to-end high-level compiler test (M-compiler / the completion goal).
//
// The kernels were written in the C-like language (examples/kernels/*.k), compiled by
// scripts/sirc.py to Sirion assembly, then assembled by scripts/asm.py to .gpubin (done by
// run_hl_tests.sh). Here we execute those .gpubin on the golden ISS with real inputs and
// check the results against known answers — proving high-level kernels compile down to the
// custom assembly, then the custom binary, and run correctly.
#include "iss.hpp"
#include "test_framework.hpp"
#include <string>
#include <cmath>
#include <cstring>

using namespace sirion;

static uint32_t f2u(float f){ uint32_t u; std::memcpy(&u,&f,4); return u; }
static float    u2f(uint32_t u){ float f; std::memcpy(&f,&u,4); return f; }

static constexpr uint32_t A = 0x1000, B = 0x2000, O = 0x3000;

static GpuBin must(const std::string& n) {
  GpuBin b; std::string e;
  if (!load_gpubin("build/" + n + ".gpubin", b, &e)) throw tf::AssertFail("load " + n + ": " + e);
  return b;
}
static Iss setup(const std::string& gpubin, uint32_t n) {
  Iss s; s.bin = must(gpubin);
  s.blockDim = {32, 1, 1}; s.gridDim = {(n + 31) / 32, 1, 1};
  s.ensure_gmem(0x40000);
  return s;
}

TEST(hl_vec_add) {                       // out[i] = a[i] + b[i]
  const uint32_t n = 100;
  Iss s = setup("hl_vec_add", n);
  for (uint32_t i = 0; i < n; ++i) { s.gst(A + 4*i, i); s.gst(B + 4*i, 2*i); }
  s.set_const(0, O); s.set_const(1, A); s.set_const(2, B); s.set_const(3, n);
  s.run();
  for (uint32_t i = 0; i < n; ++i) CHECK_EQ(s.gld(O + 4*i), 3*i);
}

TEST(hl_saxpy) {                          // out[i] = alpha*a[i] + b[i]
  const uint32_t n = 77; const int32_t alpha = 5;
  Iss s = setup("hl_saxpy", n);
  for (uint32_t i = 0; i < n; ++i) { s.gst(A + 4*i, i + 1); s.gst(B + 4*i, (uint32_t)(100 - (int)i)); }
  s.set_const(0, O); s.set_const(1, A); s.set_const(2, B); s.set_const(3, n); s.set_const(4, (uint32_t)alpha);
  s.run();
  for (uint32_t i = 0; i < n; ++i)
    CHECK_EQ((int32_t)s.gld(O + 4*i), alpha * (int32_t)(i + 1) + (100 - (int32_t)i));
}

TEST(hl_strided_sum) {                    // out[i] = sum_{j<k} a[i*k + j]
  const uint32_t n = 40, k = 6;
  Iss s = setup("hl_strided", n);
  for (uint32_t i = 0; i < n * k; ++i) s.gst(A + 4*i, i);
  s.set_const(0, O); s.set_const(1, A); s.set_const(2, n); s.set_const(3, k);
  s.run();
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t e = 0; for (uint32_t j = 0; j < k; ++j) e += i*k + j;
    CHECK_EQ(s.gld(O + 4*i), e);
  }
}

// ---- M21 language features ----

TEST(hl_saxpyf_float) {                   // float type: out[i] = a*x[i] + y[i], exact values
  const uint32_t n = 50; const float a = 1.5f;
  Iss s = setup("hl_saxpyf", n);
  for (uint32_t i = 0; i < n; ++i) { s.gst(A + 4*i, f2u((float)i + 0.5f)); s.gst(B + 4*i, f2u((float)i * 0.25f)); }
  s.set_const(0, O); s.set_const(1, A); s.set_const(2, B); s.set_const(3, n); s.set_const(4, f2u(a));
  s.run();
  for (uint32_t i = 0; i < n; ++i)
    CHECK_EQ(s.gld(O + 4*i), f2u(a * ((float)i + 0.5f) + (float)i * 0.25f));   // RTZ == IEEE here
}

TEST(hl_normalize_inline_sfu) {           // inline func + rsqrtf: 3-4-5 & 5-12-13 triangles
  Iss s = setup("hl_normalize", 2);
  s.gst(A + 0, f2u(3.0f)); s.gst(A + 4, f2u(5.0f));
  s.gst(B + 0, f2u(4.0f)); s.gst(B + 4, f2u(12.0f));
  s.set_const(0, O); s.set_const(1, A); s.set_const(2, B); s.set_const(3, 2);
  s.run();
  CHECK(std::fabs(u2f(s.gld(O + 0)) - 0.6f)        < 2e-6f);
  CHECK(std::fabs(u2f(s.gld(O + 4)) - 5.0f/13.0f)  < 2e-6f);
}

TEST(hl_reduce_shared_barrier) {          // __shared__ + barrier(): per-block sums
  const uint32_t n = 100, tpb = 64, grid = 2;
  Iss s; s.bin = must("hl_reduce_shared");
  s.blockDim = {tpb, 1, 1}; s.gridDim = {grid, 1, 1};
  s.shared_words = 4096; s.ensure_gmem(0x40000);
  for (uint32_t i = 0; i < n; ++i) s.gst(A + 4*i, i + 1);
  s.set_const(0, O); s.set_const(1, A); s.set_const(2, n);
  s.run();
  uint32_t e0 = 0, e1 = 0;
  for (uint32_t i = 0; i < 64; ++i)  e0 += i + 1;
  for (uint32_t i = 64; i < n; ++i)  e1 += i + 1;
  CHECK_EQ(s.gld(O + 0), e0);
  CHECK_EQ(s.gld(O + 4), e1);
}

TEST(hl_hist_atomics) {                   // atomic_add from the high-level language
  const uint32_t n = 200;
  Iss s = setup("hl_hist", n);
  uint32_t expect[16] = {};
  for (uint32_t i = 0; i < n; ++i) { uint32_t v = (i * 7 + 3) & 15; s.gst(A + 4*i, v); expect[v]++; }
  s.set_const(0, O); s.set_const(1, A); s.set_const(2, n);
  s.run();
  for (uint32_t b = 0; b < 16; ++b) CHECK_EQ(s.gld(O + 4*b), expect[b]);
}

TEST(hl_spill_locals) {                   // register spilling: 14 chained locals
  const uint32_t n = 8;
  Iss s; s.bin = must("hl_spill");
  s.blockDim = {32, 1, 1}; s.gridDim = {1, 1, 1};
  s.shared_words = 4096; s.ensure_gmem(0x40000);
  s.set_const(0, O); s.set_const(1, n);
  s.run();
  for (uint32_t i = 0; i < n; ++i) {
    int a=i+1, b=a*2, c=b+3, d=c*2, e=d+5, f=e*2, g=f+7, h=g*2,
        p=h+9, q=p*2, r=q+11, sx=r*2, t=sx+13, u=t*2;
    int v = u+a+b+c+d+e+f+g+h+p+q+r+sx+t;
    CHECK_EQ((int32_t)s.gld(O + 4*i), v);
  }
}

int main(int, char**) { return tf::run_all(); }
