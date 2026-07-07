// test_warp_sched.cpp — self-checking testbench for rtl/scheduler/warp_sched.sv (M7).
//
// Checks the RTL round-robin selection against a C++ golden over random ready masks +
// advances, and verifies fairness (with all warps ready and advancing every cycle, the
// scheduler visits every warp).
#include "Vwarp_sched.h"
#include "../rtl_driver.hpp"
#include "test_framework.hpp"
#include <random>

static constexpr int NW = 8;

TEST(warp_sched_matches_golden) {
  Sim<Vwarp_sched> sim; auto* d = sim.dut;
  d->ready = 0; d->advance = 0; sim.reset();

  int ptr = 0;   // golden pointer, kept in lockstep with the RTL
  std::mt19937 rng(0x5C4EDu);
  for (int it = 0; it < 5000; ++it) {
    uint8_t ready = rng() & 0xFF;
    bool adv = rng() & 1;
    d->ready = ready; d->advance = adv ? 1 : 0; d->eval();

    bool gvalid = false; int gsel = 0;
    for (int i = 1; i <= NW; ++i) { int w = (ptr + i) % NW; if (!gvalid && ((ready >> w) & 1)) { gsel = w; gvalid = true; } }
    CHECK_EQ((int)d->valid, (int)gvalid);
    if (gvalid) CHECK_EQ((int)d->sel, gsel);

    if (adv && gvalid) ptr = gsel;
    sim.tick();
  }
}

TEST(warp_sched_fairness) {
  Sim<Vwarp_sched> sim; auto* d = sim.dut;
  d->ready = 0xFF; d->advance = 1; sim.reset();
  d->ready = 0xFF; d->advance = 1;
  int seen[NW] = {0};
  for (int i = 0; i < NW * 4; ++i) { d->eval(); seen[d->sel]++; sim.tick(); }
  for (int w = 0; w < NW; ++w) CHECK(seen[w] > 0);   // no warp starved
}

int main(int, char**) { return tf::run_all(); }
