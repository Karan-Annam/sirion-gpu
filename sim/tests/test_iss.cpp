// test_iss.cpp — self-checking tests for the golden ISS (M1).
//
// Loads pre-assembled kernels from build/*.gpubin (produced by scripts/asm.py), runs
// them on the ISS over a real grid, and checks global-memory results against an
// independent C++ reference computed here. This validates the assembler <-> ISS
// agreement AND the ISS's SIMT/divergence/loop semantics end-to-end.
#include "iss.hpp"
#include "test_framework.hpp"
#include <string>

using namespace sirion;

static GpuBin must_load(const std::string& name) {
  GpuBin b; std::string err;
  if (!load_gpubin("build/" + name + ".gpubin", b, &err))
    throw tf::AssertFail("load " + name + ".gpubin: " + err);
  return b;
}

// Memory layout shared by the kernels under test.
static constexpr uint32_t A_PTR = 0x1000, B_PTR = 0x2000, OUT_PTR = 0x3000;

static Iss make_iss(const std::string& kernel, uint32_t n, uint32_t block = 64) {
  Iss s;
  s.bin = must_load(kernel);
  s.blockDim = {block, 1, 1};
  s.gridDim  = {(n + block - 1) / block, 1, 1};
  s.ensure_gmem(0x8000);
  return s;
}

TEST(iss_vec_add) {
  const uint32_t n = 100;
  Iss s = make_iss("vec_add", n);
  for (uint32_t i = 0; i < n; ++i) { s.gst(A_PTR + 4*i, i); s.gst(B_PTR + 4*i, 2*i); }
  s.set_const(0, OUT_PTR); s.set_const(1, A_PTR); s.set_const(2, B_PTR); s.set_const(3, n);
  s.run();
  for (uint32_t i = 0; i < n; ++i) CHECK_EQ(s.gld(OUT_PTR + 4*i), 3*i);
}

TEST(iss_saxpy_int) {
  const uint32_t n = 77; const int32_t alpha = 5;
  Iss s = make_iss("saxpy_int", n);
  for (uint32_t i = 0; i < n; ++i) { s.gst(A_PTR + 4*i, i + 1); s.gst(B_PTR + 4*i, 100 - i); }
  s.set_const(0, OUT_PTR); s.set_const(1, A_PTR); s.set_const(2, B_PTR);
  s.set_const(3, n); s.set_const(4, (uint32_t)alpha);
  s.run();
  for (uint32_t i = 0; i < n; ++i)
    CHECK_EQ((int32_t)s.gld(OUT_PTR + 4*i), alpha * (int32_t)(i + 1) + (int32_t)(100 - i));
}

TEST(iss_abs_divergent) {
  const uint32_t n = 96;
  Iss s = make_iss("abs", n, 32);   // 3 warps; mix of signs -> real divergence
  for (uint32_t i = 0; i < n; ++i) {
    int32_t x = ((int32_t)i % 2) ? -(int32_t)i : (int32_t)i;   // alternate sign
    s.gst(A_PTR + 4*i, (uint32_t)x);
  }
  s.set_const(0, OUT_PTR); s.set_const(1, A_PTR); s.set_const(2, n);
  s.run();
  for (uint32_t i = 0; i < n; ++i) {
    int32_t x = ((int32_t)i % 2) ? -(int32_t)i : (int32_t)i;
    CHECK_EQ((int32_t)s.gld(OUT_PTR + 4*i), x < 0 ? -x : x);
  }
  CHECK(s.stats.divergent_branches > 0);   // the sign branch must actually diverge
}

TEST(iss_loop_reduce) {
  const uint32_t n = 50, K = 8;
  Iss s = make_iss("loop_reduce", n);
  for (uint32_t i = 0; i < n * K; ++i) s.gst(A_PTR + 4*i, i);   // a[j] = j
  s.set_const(0, OUT_PTR); s.set_const(1, A_PTR); s.set_const(2, n); s.set_const(3, K);
  s.run();
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t expect = 0;
    for (uint32_t k = 0; k < K; ++k) expect += i * K + k;
    CHECK_EQ(s.gld(OUT_PTR + 4*i), expect);
  }
}

// Range guard must leave out-of-range outputs untouched (predication correctness).
TEST(iss_range_guard) {
  const uint32_t n = 10;
  Iss s = make_iss("vec_add", n, 32);   // 1 warp, 32 lanes, only 10 active
  for (uint32_t i = 0; i < 32; ++i) { s.gst(A_PTR + 4*i, i); s.gst(B_PTR + 4*i, 2*i);
                                      s.gst(OUT_PTR + 4*i, 0xDEADBEEF); }
  s.set_const(0, OUT_PTR); s.set_const(1, A_PTR); s.set_const(2, B_PTR); s.set_const(3, n);
  s.run();
  for (uint32_t i = 0; i < n; ++i)  CHECK_EQ(s.gld(OUT_PTR + 4*i), 3*i);
  for (uint32_t i = n; i < 32; ++i) CHECK_EQ(s.gld(OUT_PTR + 4*i), 0xDEADBEEF);  // untouched
}

int main(int, char**) { return tf::run_all(); }
