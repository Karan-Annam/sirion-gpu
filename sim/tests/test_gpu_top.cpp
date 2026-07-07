// test_gpu_top.cpp — self-checking testbench for rtl/gpu_top.sv (M12).
//
// M12 moves block/grid dispatch from the C++ testbench into HARDWARE: the host preloads
// program/constants/memory, writes ONE launch descriptor (grid, tpb, entry), pulses launch,
// and the cta_dispatch walks every block of the grid on the CU without host involvement.
// Each test then diffs final global memory (and, for the barrier kernel, every warp's
// registers) against the golden ISS AND a closed-form known answer (unbiased).
#include "gpu_tb.hpp"
#include "../iss/iss.hpp"
#include <string>
#include <cmath>
#include <cstring>

using namespace sirion;

// vec_add over a grid: out[i] = a[i] + b[i], launched as ONE hardware grid launch.
TEST(gpu_top_vec_add_grid) {
  const uint32_t n = 100, A = 0x1000, B = 0x2000, O = 0x3000;
  const uint32_t block = 32, grid = (n + block - 1) / block;
  auto a_val = [](uint32_t i) { return i + 1; };
  auto b_val = [](uint32_t i) { return 3 * i + 2; };

  GpuBin bin; std::string err;
  if (!load_gpubin("build/vec_add.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Iss iss; iss.bin = bin;
  iss.blockDim = {block, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.ensure_gmem(0x8000);
  for (uint32_t i = 0; i < n; ++i) { iss.gst(A + 4*i, a_val(i)); iss.gst(B + 4*i, b_val(i)); }
  iss.set_const(0, O); iss.set_const(1, A); iss.set_const(2, B); iss.set_const(3, n);
  iss.run();
  for (uint32_t i = 0; i < n; ++i) CHECK_EQ(iss.gld(O + 4*i), a_val(i) + b_val(i));

  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, A); g.set_const(2, B); g.set_const(3, n);
  for (uint32_t i = 0; i < n; ++i) { g.store_gmem(A + 4*i, a_val(i)); g.store_gmem(B + 4*i, b_val(i)); }
  g.launch_grid(grid, block);

  for (uint32_t i = 0; i < n; ++i) {
    CHECK_EQ(g.load_gmem(O + 4*i), iss.gld(O + 4*i));
    CHECK_EQ(g.load_gmem(O + 4*i), a_val(i) + b_val(i));
  }
}

// Multi-warp barrier kernel across a hardware-dispatched grid (regs of last block diffed too).
static void gpu_top_shuffle(uint32_t tpb, uint32_t grid) {
  const uint32_t OUTB = 0x400;
  const uint32_t nwarps = (tpb + 31) / 32;
  auto expect = [&](uint32_t b, uint32_t t) { return (uint32_t)(b * tpb + (tpb - 1 - t)); };

  GpuBin bin; std::string err;
  if (!load_gpubin("build/block_shuffle.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Iss iss; iss.bin = bin;
  iss.blockDim = {tpb, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.shared_words = tpb;
  iss.ensure_gmem(OUTB + 4 * tpb * grid);
  iss.set_const(0, OUTB);
  iss.cap_enable = true; iss.cap_block = grid - 1; iss.cap_warp = 0;
  iss.run();

  Gpu g;
  g.load_program(bin);
  g.set_const(0, OUTB);
  g.launch_grid(grid, tpb);

  for (uint32_t b = 0; b < grid; ++b)
    for (uint32_t t = 0; t < tpb; ++t) {
      CHECK_EQ(g.load_gmem(OUTB + 4*(b*tpb + t)), iss.gld(OUTB + 4*(b*tpb + t)));
      CHECK_EQ(g.load_gmem(OUTB + 4*(b*tpb + t)), expect(b, t));
    }
  for (uint32_t w = 0; w < nwarps; ++w) {
    g.d->dbg_warp = (uint8_t)w;
    for (int r = 0; r < NUM_VREGS; ++r) {
      g.d->dbg_addr = r; g.sim.tick();
      for (int lane = 0; lane < WARP_SIZE; ++lane)
        CHECK_EQ(g.d->dbg_data[lane], iss.cap_warps[w].regs[lane][r]);
    }
  }
}

TEST(gpu_top_shuffle_grid)   { gpu_top_shuffle(64, 3); }
TEST(gpu_top_shuffle_partial){ gpu_top_shuffle(100, 2); }

// M13 pipeline throughput: run an 8-warp COMPUTE-bound block (the divergent-loop ALU kernel,
// no memory ops) and report IPC. The barrel pipeline must sustain far above the multicycle
// core's ~0.2 IPC ceiling when many warps are ready; memory-bound IPC is M14's job.
TEST(gpu_top_pipeline_ipc) {
  const uint32_t tpb = 256, grid = 2;
  GpuBin bin; std::string err;
  if (!load_gpubin("build/m4_loop.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Iss iss; iss.bin = bin;
  iss.blockDim = {tpb, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.cap_enable = true; iss.cap_block = grid - 1; iss.cap_warp = 0;
  iss.run();

  Gpu g;
  g.load_program(bin);
  uint32_t c0 = g.d->perf_cycles, i0 = g.d->perf_insns;
  g.launch_grid(grid, tpb);
  uint32_t cycles = g.d->perf_cycles - c0, insns = g.d->perf_insns - i0;

  // unbiased: every warp of the last block matches the ISS AND the closed-form loop sum
  for (uint32_t w = 0; w < tpb / 32; ++w) {
    g.d->dbg_warp = (uint8_t)w;
    for (int r = 0; r < NUM_VREGS; ++r) {
      g.d->dbg_addr = r; g.sim.tick();
      for (int lane = 0; lane < WARP_SIZE; ++lane)
        CHECK_EQ(g.d->dbg_data[lane], iss.cap_warps[w].regs[lane][r]);
    }
    g.d->dbg_addr = 2; g.sim.tick();
    for (int lane = 0; lane < WARP_SIZE; ++lane)
      CHECK_EQ(g.d->dbg_data[lane], (uint32_t)(lane * (lane + 1) / 2));
  }

  double ipc = (double)insns / (double)cycles;
  std::printf("  [perf] insns=%u cycles=%u IPC=%.3f (multicycle core was ~0.2)\n",
              insns, cycles, ipc);
  CHECK(ipc > 0.50);
}

// Back-to-back launches: two different grids on the same device without reset.
TEST(gpu_top_relaunch) {
  const uint32_t OUTB = 0x400, tpb = 64;
  GpuBin bin; std::string err;
  if (!load_gpubin("build/block_shuffle.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Gpu g;
  g.load_program(bin);
  g.set_const(0, OUTB);
  g.launch_grid(2, tpb);   // first grid
  CHECK_EQ(g.d->perf_blocks, 2u);
  g.launch_grid(3, tpb);   // relaunch: overwrites out[] with the 3-block result
  CHECK_EQ(g.d->perf_blocks, 5u);
  for (uint32_t b = 0; b < 3; ++b)
    for (uint32_t t = 0; t < tpb; ++t)
      CHECK_EQ(g.load_gmem(OUTB + 4*(b*tpb + t)), b*tpb + (tpb - 1 - t));
}

// M14 coalescer: with n=128 (4 full blocks, all lanes active) every 32-lane LOAD must
// become EXACTLY 32/LINE_WORDS = 4 L1 line transactions — not 32 serial lane accesses.
// (After M17 the L1 is write-through: stores forward to the L2 without touching the L1
// hit/miss counters, so only the 2 loads of vec_add's 3 memory ops are counted here.)
TEST(gpu_top_coalescing) {
  const uint32_t n = 128, A = 0x1000, B = 0x2000, O = 0x3000;
  const uint32_t block = 32, grid = n / block;
  GpuBin bin; std::string err;
  if (!load_gpubin("build/vec_add.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, A); g.set_const(2, B); g.set_const(3, n);
  for (uint32_t i = 0; i < n; ++i) { g.store_gmem(A + 4*i, i + 1); g.store_gmem(B + 4*i, 2*i); }
  uint32_t m0 = g.d->perf_memops, h0 = g.d->perf_l1_hits, x0 = g.d->perf_l1_misses;
  g.launch_grid(grid, block);
  uint32_t memops = g.d->perf_memops - m0;            // 3 per block (LDG, LDG, STG)
  uint32_t loads  = memops * 2 / 3;                   // the counted (load) transactions
  uint32_t trans  = (g.d->perf_l1_hits - h0) + (g.d->perf_l1_misses - x0);
  std::printf("  [perf] memops=%u load_transactions=%u (%.1f lines/load) hits=%u misses=%u\n",
              memops, trans, (double)trans / loads,
              g.d->perf_l1_hits - h0, g.d->perf_l1_misses - x0);
  CHECK_EQ(trans, 4u * loads);                        // perfect coalescing
  for (uint32_t i = 0; i < n; ++i) CHECK_EQ(g.load_gmem(O + 4*i), (i + 1) + 2*i);
}

// M14 locality: loop_reduce walks each lane sequentially through memory, so consecutive
// iterations reuse the same cache line — the hit rate must be high (not a transparent no-op).
TEST(gpu_top_cache_hits) {
  const uint32_t n = 32, K = 16, A = 0x1000, O = 0x3000;
  GpuBin bin; std::string err;
  if (!load_gpubin("build/loop_reduce.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Iss iss; iss.bin = bin;
  iss.blockDim = {32, 1, 1}; iss.gridDim = {1, 1, 1};
  iss.ensure_gmem(0x8000);
  for (uint32_t i = 0; i < n * K; ++i) iss.gst(A + 4*i, i % 7 + 1);
  iss.set_const(0, O); iss.set_const(1, A); iss.set_const(2, n); iss.set_const(3, K);
  iss.run();

  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, A); g.set_const(2, n); g.set_const(3, K);
  for (uint32_t i = 0; i < n * K; ++i) g.store_gmem(A + 4*i, i % 7 + 1);
  uint32_t h0 = g.d->perf_l1_hits, x0 = g.d->perf_l1_misses;
  g.launch_grid(1, 32);
  uint32_t hits = g.d->perf_l1_hits - h0, misses = g.d->perf_l1_misses - x0;
  double rate = (double)hits / (double)(hits + misses);
  std::printf("  [perf] l1 hits=%u misses=%u hit-rate=%.1f%%\n", hits, misses, 100.0 * rate);
  for (uint32_t i = 0; i < n; ++i) CHECK_EQ(g.load_gmem(O + 4*i), iss.gld(O + 4*i));
  CHECK(rate > 0.5);                                  // sequential reuse must mostly hit
}

// M16 atomics: a real multi-block histogram — hundreds of threads increment 16 shared bins
// concurrently across warps AND blocks; only true atomic RMW gives the closed-form counts.
TEST(gpu_top_histogram_atomic) {
  const uint32_t n = 1000, BINS = 16, H = 0x200, DATA = 0x1000;
  const uint32_t tpb = 256, grid = (n + tpb - 1) / tpb;
  GpuBin bin; std::string err;
  if (!load_gpubin("build/histogram.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  uint32_t expect[BINS] = {};
  auto val = [&](uint32_t i) { return (i * 7 + 3) % BINS; };
  for (uint32_t i = 0; i < n; ++i) expect[val(i)]++;

  Iss iss; iss.bin = bin;
  iss.blockDim = {tpb, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.ensure_gmem(0x8000);
  for (uint32_t i = 0; i < n; ++i) iss.gst(DATA + 4*i, val(i));
  iss.set_const(0, H); iss.set_const(1, DATA); iss.set_const(2, n);
  iss.run();
  for (uint32_t b = 0; b < BINS; ++b) CHECK_EQ(iss.gld(H + 4*b), expect[b]);

  Gpu g;
  g.load_program(bin);
  g.set_const(0, H); g.set_const(1, DATA); g.set_const(2, n);
  for (uint32_t i = 0; i < n; ++i) g.store_gmem(DATA + 4*i, val(i));
  g.launch_grid(grid, tpb);
  for (uint32_t b = 0; b < BINS; ++b) {
    CHECK_EQ(g.load_gmem(H + 4*b), expect[b]);          // RTL == closed form
    CHECK_EQ(g.load_gmem(H + 4*b), iss.gld(H + 4*b));   // RTL == ISS
  }
}

// M16 SFU through the datapath: out[i] = rsqrt(in[i]), RTL == ISS bit-exact and within
// tolerance of libm (unbiased).
TEST(gpu_top_sfu_rsqrt) {
  const uint32_t n = 100, INB = 0x1000, O = 0x3000, tpb = 32, grid = (n + tpb - 1) / tpb;
  auto f2u = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
  auto u2f = [](uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; };
  auto in_val = [](uint32_t i) { return 0.25f + (float)i * 0.5f; };

  GpuBin bin; std::string err;
  if (!load_gpubin("build/rsqrt_map.gpubin", bin, &err)) throw tf::AssertFail("load: " + err);

  Iss iss; iss.bin = bin;
  iss.blockDim = {tpb, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.ensure_gmem(0x8000);
  for (uint32_t i = 0; i < n; ++i) iss.gst(INB + 4*i, f2u(in_val(i)));
  iss.set_const(0, O); iss.set_const(1, INB); iss.set_const(2, n);
  iss.run();

  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, INB); g.set_const(2, n);
  for (uint32_t i = 0; i < n; ++i) g.store_gmem(INB + 4*i, f2u(in_val(i)));
  g.launch_grid(grid, tpb);
  for (uint32_t i = 0; i < n; ++i) {
    CHECK_EQ(g.load_gmem(O + 4*i), iss.gld(O + 4*i));   // bit-exact vs ISS
    float want = 1.0f / std::sqrt(in_val(i));
    CHECK(std::fabs(u2f(g.load_gmem(O + 4*i)) - want) <= 5e-7f * want);
  }
}

int main(int, char**) { return tf::run_all(); }
