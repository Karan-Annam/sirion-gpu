// test_l1_cache.cpp — self-checking testbench for rtl/cache/l1_cache.sv (M7).
//
// The cache must be *transparent*: every read returns what a flat memory would (the last
// value written to that word). The testbench models the backing memory (line-granular,
// combinational read + synchronous write) and a golden flat array, issues thousands of
// random reads/writes across a range large enough to force evictions + write-backs, and
// checks every read. Also sanity-checks the hit/miss counters.
#include "Vl1_cache.h"
#include "../rtl_driver.hpp"   // sc_time_stamp stub
#include "test_framework.hpp"
#include <verilated.h>
#include <vector>
#include <random>

static constexpr int LW = 4;      // LINE_WORDS
static constexpr int NW = 1024;   // backing-store words (forces conflicts/evictions)

TEST(l1_cache_transparent) {
  VerilatedContext ctx;
  Vl1_cache* d = new Vl1_cache(&ctx);
  std::vector<uint32_t> backing(NW), flat(NW);
  for (int i = 0; i < NW; ++i) backing[i] = flat[i] = 0xC0DE0000u | i;

  auto service = [&]{                         // drive combinational line read from mem_raddr
    uint32_t base = d->mem_raddr >> 2;
    for (int i = 0; i < LW; ++i) d->mem_rline[i] = (base + i < (uint32_t)NW) ? backing[base + i] : 0;
  };
  auto tick = [&]{
    d->clk = 0; d->eval(); service(); d->eval();
    d->clk = 1; d->eval(); service(); d->eval();
    if (d->mem_we) {                          // capture a write-back
      uint32_t wb = d->mem_waddr >> 2;
      for (int i = 0; i < LW; ++i) if (wb + i < (uint32_t)NW) backing[wb + i] = d->mem_wline[i];
    }
  };
  auto reset = [&]{ d->rst = 1; d->req_valid = 0; for (int i=0;i<6;++i) tick(); d->rst = 0; };
  auto access = [&](uint32_t w, bool we, uint32_t wd) -> uint32_t {
    d->req_addr = w << 2; d->req_we = we; d->req_wdata = wd; d->req_valid = 1;
    tick(); d->req_valid = 0;
    int c = 0; while (!d->resp_ready && c < 100) { tick(); ++c; }
    uint32_t r = d->resp_rdata; tick();
    return r;
  };

  reset();
  std::mt19937 rng(0xCAC4E);
  for (int it = 0; it < 8000; ++it) {
    uint32_t w = rng() % NW;
    if (rng() & 1) {                          // read
      CHECK_EQ(access(w, false, 0), flat[w]);
    } else {                                  // write
      uint32_t v = rng();
      access(w, true, v);
      flat[w] = v;
    }
  }
  // read the whole range back — must all match the golden (also exercises many evictions)
  for (int w = 0; w < NW; ++w) CHECK_EQ(access(w, false, 0), flat[w]);

  CHECK(d->perf_hits + d->perf_misses > 0);
  CHECK(d->perf_hits > 0);                    // repeated accesses must hit
  std::printf("  cache: %u hits, %u misses\n", d->perf_hits, d->perf_misses);
  delete d;
}

int main(int, char**) { return tf::run_all(); }
