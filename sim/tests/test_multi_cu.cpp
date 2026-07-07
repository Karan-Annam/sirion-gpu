// test_multi_cu.cpp — multi-CU scale-out suite (M17). Built with -GNUM_CU=4.
//
// Blocks of one launch really run CONCURRENTLY on 4 compute units sharing one L2:
//  * correctness: vec_add + the barrier shuffle + FP saxpy across many blocks match the
//    ISS bit-exact (block-disjoint outputs; the write-through L1s + word-merging L2 make
//    same-line writes from different CUs safe);
//  * atomics: a 4-CU histogram hammers the same 16 bins from all CUs at once — the L2
//    is the serialization point; counts must be exact;
//  * scaling: a compute-heavy grid must run substantially faster on 4 CUs than the
//    1-CU wall time of the same work.
#include "gpu_tb.hpp"
#include "../iss/iss.hpp"
#include <string>
#include <cmath>
#include <cstring>

using namespace sirion;

TEST(mcu_vec_add) {
  const uint32_t n = 512, A = 0x1000, B = 0x2000, O = 0x3000;
  const uint32_t block = 32, grid = n / block;               // 16 blocks over 4 CUs
  GpuBin bin; std::string err;
  if (!load_gpubin("build/vec_add.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, A); g.set_const(2, B); g.set_const(3, n);
  for (uint32_t i = 0; i < n; ++i) { g.store_gmem(A + 4*i, i + 1); g.store_gmem(B + 4*i, 3*i + 2); }
  g.launch_grid(grid, block);
  for (uint32_t i = 0; i < n; ++i)
    CHECK_EQ(g.load_gmem(O + 4*i), (i + 1) + (3*i + 2));     // closed form
}

TEST(mcu_barrier_shuffle) {
  const uint32_t OUTB = 0x400, tpb = 64, grid = 8;           // 8 blocks over 4 CUs
  GpuBin bin; std::string err;
  if (!load_gpubin("build/block_shuffle.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);
  Gpu g;
  g.load_program(bin);
  g.set_const(0, OUTB);
  g.launch_grid(grid, tpb);
  for (uint32_t b = 0; b < grid; ++b)
    for (uint32_t t = 0; t < tpb; ++t)
      CHECK_EQ(g.load_gmem(OUTB + 4*(b*tpb + t)), b*tpb + (tpb - 1 - t));
}

TEST(mcu_histogram_atomics_l2) {
  const uint32_t n = 2000, BINS = 16, H = 0x200, DATA = 0x1000;
  const uint32_t tpb = 128, grid = (n + tpb - 1) / tpb;      // 16 blocks over 4 CUs
  GpuBin bin; std::string err;
  if (!load_gpubin("build/histogram.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  uint32_t expect[BINS] = {};
  auto val = [&](uint32_t i) { return (i * 13 + 5) % BINS; };
  for (uint32_t i = 0; i < n; ++i) expect[val(i)]++;

  Gpu g;
  g.load_program(bin);
  g.set_const(0, H); g.set_const(1, DATA); g.set_const(2, n);
  for (uint32_t i = 0; i < n; ++i) g.store_gmem(DATA + 4*i, val(i));
  g.launch_grid(grid, tpb);
  for (uint32_t b = 0; b < BINS; ++b) CHECK_EQ(g.load_gmem(H + 4*b), expect[b]);
}

TEST(mcu_saxpy_float) {
  const uint32_t n = 300, X = 0x1000, Y = 0x2000, O = 0x3000;
  const float a = 2.5f;
  auto f2u = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
  auto x_val = [](uint32_t i) { return (float)i * 0.5f; };
  auto y_val = [](uint32_t i) { return (float)i + 0.25f; };
  const uint32_t block = 32, grid = (n + block - 1) / block; // 10 blocks over 4 CUs
  GpuBin bin; std::string err;
  if (!load_gpubin("build/saxpy_float.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);
  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, X); g.set_const(2, Y); g.set_const(3, n); g.set_const(4, f2u(a));
  for (uint32_t i = 0; i < n; ++i) { g.store_gmem(X + 4*i, f2u(x_val(i))); g.store_gmem(Y + 4*i, f2u(y_val(i))); }
  g.launch_grid(grid, block);
  for (uint32_t i = 0; i < n; ++i)
    CHECK_EQ(g.load_gmem(O + 4*i), f2u(a * x_val(i) + y_val(i)));   // exact values
}

TEST(mcu_scaling) {
  // compute-heavy divergent-loop grid: 16 blocks of 256 threads; compare 4-CU wall time
  // against the same launch's serial estimate (sum of per-CU busy cycles ~= 1-CU time).
  const uint32_t tpb = 256, grid = 16;
  GpuBin bin; std::string err;
  if (!load_gpubin("build/m4_loop.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);
  Gpu g;
  g.load_program(bin);
  uint32_t wall = g.launch_grid(grid, tpb);
  // perf_insns aggregates all CUs; at IPC<=1 per CU, 1 CU would need >= insns cycles.
  uint32_t serial_lb = g.d->perf_insns;              // lower bound on 1-CU wall time
  double speedup = (double)serial_lb / (double)wall;
  std::printf("  [perf] wall=%u cycles, serial-lower-bound=%u, speedup >= %.2fx (4 CUs)\n",
              wall, serial_lb, speedup);
  CHECK(speedup > 2.0);                              // must scale well past 2x on 4 CUs
}

int main(int, char**) { return tf::run_all(); }
