// test_cu_core.cpp — kernel-correctness suite for the Sirion GPU (M3–M16 kernels).
//
// Runs every kernel on BOTH the golden ISS and the RTL GPU (gpu_top, single-CU build:
// after M17 the CU's global memory lives behind L1 -> L2 inside the top level), then
// checks final registers / predicates / memory bit-identical to the ISS AND against
// closed-form known answers (unbiased).
#include "gpu_tb.hpp"
#include "../iss/iss.hpp"
#include <string>
#include <functional>
#include <algorithm>
#include <cstring>

using namespace sirion;

struct Ctx { uint32_t tpb, grid; };

// `ground` (optional) independently checks the ISS output against a known-answer, so the
// test verifies RTL==ISS *and* ISS==ground-truth (not just RTL/ISS self-consistency).
static void run_case(const std::string& kernel, Ctx c,
                     std::function<void(const Iss&)> ground = nullptr) {
  GpuBin bin; std::string err;
  if (!load_gpubin("build/" + kernel + ".gpubin", bin, &err))
    throw tf::AssertFail("load " + kernel + ": " + err);

  // ---- golden ISS (capture the LAST block; its registers stay resident in the RTL) ----
  Iss iss;
  iss.bin = bin;
  iss.blockDim = {c.tpb, 1, 1};
  iss.gridDim  = {c.grid, 1, 1};
  iss.cap_enable = true; iss.cap_block = c.grid - 1; iss.cap_warp = 0;
  iss.shared_words = 256;
  iss.run();
  if (ground) ground(iss);   // ISS vs known-answer (unbiased check)

  // ---- RTL GPU ----
  Gpu g;
  g.load_program(bin);
  g.launch_grid(c.grid, c.tpb);

  // compare the whole register file of warp 0 of the last block, all lanes
  for (int r = 0; r < NUM_VREGS; ++r) {
    g.read_reg(0, 0, r);
    for (int lane = 0; lane < WARP_SIZE; ++lane)
      CHECK_EQ(g.d->dbg_data[lane], iss.cap_regs[lane][r]);
  }
  CHECK_EQ(g.d->dbg_pred1, iss.cap_pred[1]);
  CHECK_EQ(g.d->dbg_pred2, iss.cap_pred[2]);
  CHECK_EQ(g.d->dbg_pred3, iss.cap_pred[3]);
}

// ---- memory kernels (vec_add / saxpy_int) over a hardware-dispatched grid ----
static void run_mem(const std::string& kernel, uint32_t n, bool saxpy, int32_t alpha) {
  const uint32_t A = 0x1000, B = 0x2000, O = 0x3000;
  auto a_val = [](uint32_t i) { return i + 1; };
  auto b_val = [](uint32_t i) { return 3 * i + 2; };
  auto expect = [&](uint32_t i) -> uint32_t {
    return saxpy ? (uint32_t)(alpha * (int32_t)a_val(i) + (int32_t)b_val(i))
                 : a_val(i) + b_val(i);
  };

  GpuBin bin; std::string err;
  if (!load_gpubin("build/" + kernel + ".gpubin", bin, &err))
    throw tf::AssertFail("load " + kernel + ": " + err);
  const uint32_t block = 32, grid = (n + block - 1) / block;

  Iss iss; iss.bin = bin;
  iss.blockDim = {block, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.ensure_gmem(0x8000);
  for (uint32_t i = 0; i < n; ++i) { iss.gst(A + 4*i, a_val(i)); iss.gst(B + 4*i, b_val(i)); }
  iss.set_const(0, O); iss.set_const(1, A); iss.set_const(2, B); iss.set_const(3, n);
  if (saxpy) iss.set_const(4, (uint32_t)alpha);
  iss.run();
  for (uint32_t i = 0; i < n; ++i) CHECK_EQ(iss.gld(O + 4*i), expect(i));   // ISS vs known-answer

  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, A); g.set_const(2, B); g.set_const(3, n);
  if (saxpy) g.set_const(4, (uint32_t)alpha);
  for (uint32_t i = 0; i < n; ++i) { g.store_gmem(A + 4*i, a_val(i)); g.store_gmem(B + 4*i, b_val(i)); }
  g.launch_grid(grid, block);

  for (uint32_t i = 0; i < n; ++i) {
    CHECK_EQ(g.load_gmem(O + 4*i), iss.gld(O + 4*i));
    CHECK_EQ(g.load_gmem(O + 4*i), expect(i));
  }
}

// Ground-truth checkers (ISS output vs a known answer).
static void gt_arith(const Iss& iss) {
  for (uint32_t l = 0; l < 32; ++l) {
    uint32_t r2 = l * 3 + 7, r3 = l << 2;
    CHECK_EQ(iss.cap_regs[l][2], r2);
    CHECK_EQ(iss.cap_regs[l][4], r2 & r3);
    CHECK_EQ(iss.cap_regs[l][9], (uint32_t)std::max((int32_t)r2, (int32_t)r3));
  }
}
static void gt_abs(const Iss& iss) {
  for (int l = 0; l < 32; ++l) {
    int x = l - 16, ax = x < 0 ? -x : x;
    CHECK_EQ((int)iss.cap_regs[l][1], ax);
    CHECK_EQ((int)iss.cap_regs[l][3], ax + 1);
  }
}
static void gt_nested(const Iss& iss) {
  for (int l = 0; l < 32; ++l) {
    uint32_t e = (l >= 16) ? 1000u : ((l % 2 == 0) ? 1101u : 1201u);
    CHECK_EQ(iss.cap_regs[l][5], e);
  }
}
// M3: straight-line integer
TEST(cu_core_arith)   { run_case("m3_arith", {32, 1}, gt_arith); }
TEST(cu_core_pred)    { run_case("m3_pred",  {32, 1}); }
TEST(cu_core_ids)     { run_case("m3_ids",   {32, 4}); }   // 4 blocks; last block's ids diffed
TEST(cu_core_partial) { run_case("m3_arith", {20, 1}); }   // 20-lane warp (partial present)

// M4: SIMT control flow (divergence + reconvergence) — with known-answer checks
TEST(cu_core_m4_abs)    { run_case("m4_abs",    {32, 1}, gt_abs); }
TEST(cu_core_m4_nested) { run_case("m4_nested", {32, 1}, gt_nested); }
TEST(cu_core_m4_loop)   { run_case("m4_loop",   {32, 1},
  [](const Iss& iss){ for (int l=0;l<32;++l) CHECK_EQ(iss.cap_regs[l][2], (uint32_t)(l*(l+1)/2)); }); }
TEST(cu_core_m4_loop8)  { run_case("m4_loop",   {8, 1},
  [](const Iss& iss){ for (int l=0;l<8;++l)  CHECK_EQ(iss.cap_regs[l][2], (uint32_t)(l*(l+1)/2)); }); }

// M5: memory kernels end-to-end (global memory diffed vs ISS + known-answer)
TEST(cu_core_m5_vec_add) { run_mem("vec_add",   100, false, 0); }
TEST(cu_core_m5_saxpy)   { run_mem("saxpy_int",  77, true,  5); }

// Compiler: high-level language -> asm -> .gpubin -> hardware
TEST(cu_core_hl_vec_add) { run_mem("hl_vec_add", 100, false, 0); }

// M6: shared memory + barrier (single warp)
TEST(cu_core_m6_shared) { run_case("m6_shared", {32, 1},
  [](const Iss& iss){ for (int l=0;l<32;++l) CHECK_EQ(iss.cap_regs[l][5], (uint32_t)((31-l)*(31-l))); }); }

// ---- M11: multi-warp block with a REAL cross-warp barrier (see block_shuffle.s) ----
static void run_block_shuffle(uint32_t tpb, uint32_t grid) {
  const uint32_t OUTB = 0x400;
  const uint32_t nwarps = (tpb + 32u - 1) / 32u;
  auto expect = [&](uint32_t b, uint32_t t) -> uint32_t {
    return (uint32_t)(b * tpb + (tpb - 1 - t));
  };

  GpuBin bin; std::string err;
  if (!load_gpubin("build/block_shuffle.gpubin", bin, &err))
    throw tf::AssertFail(std::string("load block_shuffle: ") + err);

  Iss iss; iss.bin = bin;
  iss.blockDim = {tpb, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.shared_words = tpb;
  iss.ensure_gmem(OUTB + 4 * tpb * grid);
  iss.set_const(0, OUTB);
  iss.cap_enable = true; iss.cap_block = grid - 1; iss.cap_warp = 0;
  iss.run();
  for (uint32_t b = 0; b < grid; ++b)
    for (uint32_t t = 0; t < tpb; ++t)
      CHECK_EQ(iss.gld(OUTB + 4*(b*tpb + t)), expect(b, t));
  CHECK_EQ((uint32_t)iss.cap_warps.size(), nwarps);

  Gpu g;
  g.load_program(bin);
  g.set_const(0, OUTB);
  g.launch_grid(grid, tpb);

  for (uint32_t b = 0; b < grid; ++b)
    for (uint32_t t = 0; t < tpb; ++t) {
      CHECK_EQ(g.load_gmem(OUTB + 4*(b*tpb + t)), iss.gld(OUTB + 4*(b*tpb + t)));
      CHECK_EQ(g.load_gmem(OUTB + 4*(b*tpb + t)), expect(b, t));
    }
  for (uint32_t w = 0; w < nwarps; ++w)
    for (int r = 0; r < NUM_VREGS; ++r) {
      g.read_reg(0, w, r);
      for (int lane = 0; lane < WARP_SIZE; ++lane)
        CHECK_EQ(g.d->dbg_data[lane], iss.cap_warps[w].regs[lane][r]);
    }
}

TEST(cu_core_m11_shuffle64)       { run_block_shuffle(64,  3); }
TEST(cu_core_m11_shuffle256)      { run_block_shuffle(256, 2); }
TEST(cu_core_m11_shuffle_partial) { run_block_shuffle(100, 2); }

// ---- M15: floating point through the datapath (FFMA, exactly-representable values) ----
TEST(cu_core_m15_saxpy_float) {
  const uint32_t n = 77, X = 0x1000, Y = 0x2000, O = 0x3000;
  const float a = 1.5f;
  auto f2u = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
  auto x_val = [](uint32_t i) { return (float)i + 0.5f; };
  auto y_val = [](uint32_t i) { return (float)i * 0.25f; };
  const uint32_t block = 32, grid = (n + block - 1) / block;

  GpuBin bin; std::string err;
  if (!load_gpubin("build/saxpy_float.gpubin", bin, &err))
    throw tf::AssertFail("load saxpy_float: " + err);

  Iss iss; iss.bin = bin;
  iss.blockDim = {block, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.ensure_gmem(0x8000);
  for (uint32_t i = 0; i < n; ++i) { iss.gst(X + 4*i, f2u(x_val(i))); iss.gst(Y + 4*i, f2u(y_val(i))); }
  iss.set_const(0, O); iss.set_const(1, X); iss.set_const(2, Y); iss.set_const(3, n);
  iss.set_const(4, f2u(a));
  iss.run();
  for (uint32_t i = 0; i < n; ++i)
    CHECK_EQ(iss.gld(O + 4*i), f2u(a * x_val(i) + y_val(i)));   // ISS == real IEEE

  Gpu g;
  g.load_program(bin);
  g.set_const(0, O); g.set_const(1, X); g.set_const(2, Y); g.set_const(3, n); g.set_const(4, f2u(a));
  for (uint32_t i = 0; i < n; ++i) { g.store_gmem(X + 4*i, f2u(x_val(i))); g.store_gmem(Y + 4*i, f2u(y_val(i))); }
  g.launch_grid(grid, block);
  for (uint32_t i = 0; i < n; ++i) {
    CHECK_EQ(g.load_gmem(O + 4*i), iss.gld(O + 4*i));
    CHECK_EQ(g.load_gmem(O + 4*i), f2u(a * x_val(i) + y_val(i)));
  }
}

int main(int, char**) { return tf::run_all(); }
