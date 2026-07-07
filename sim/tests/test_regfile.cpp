// test_regfile.cpp — self-checking testbench for rtl/compute/regfile.sv (M2).
//
// Golden model: gold[reg][lane]. Exercises predicated (per-lane masked) writes,
// 1-cycle registered reads with NO write bypass (a concurrent read+write to the same
// register returns the pre-write value), and bank-conflict detection.
#include "Vregfile.h"
#include "../rtl_driver.hpp"
#include "test_framework.hpp"
#include <random>
#include <cstring>

static constexpr int NV = 16, WS = 32, NB = 4;

TEST(regfile_directed) {
  Sim<Vregfile> sim; auto* d = sim.dut;
  d->we = 0; d->ren0 = 0; d->ren1 = 0; d->wmask = 0;
  d->waddr = 0; d->raddr0 = 0; d->raddr1 = 0;
  for (int l = 0; l < WS; ++l) d->wdata[l] = 0;
  sim.reset();

  // write reg 3 = {lane*7} on all lanes
  d->we = 1; d->waddr = 3; d->wmask = 0xFFFFFFFF;
  for (int l = 0; l < WS; ++l) d->wdata[l] = l * 7;
  sim.tick();
  // read it back on port 0
  d->we = 0; d->ren0 = 1; d->raddr0 = 3;
  sim.tick();
  for (int l = 0; l < WS; ++l) CHECK_EQ(d->rdata0[l], (uint32_t)(l * 7));

  // masked write: only even lanes of reg 3 updated to 0xABCD
  d->we = 1; d->waddr = 3; d->wmask = 0x55555555;
  for (int l = 0; l < WS; ++l) d->wdata[l] = 0xABCD;
  d->ren0 = 0;
  sim.tick();
  d->we = 0; d->ren0 = 1; d->raddr0 = 3;
  sim.tick();
  for (int l = 0; l < WS; ++l)
    CHECK_EQ(d->rdata0[l], (l % 2 == 0) ? 0xABCDu : (uint32_t)(l * 7));
}

TEST(regfile_bank_conflict) {
  Sim<Vregfile> sim; auto* d = sim.dut;
  d->we = 0; d->ren0 = 1; d->ren1 = 1;
  // same bank (0 and 4 -> both bank 0), differing regs -> conflict
  d->raddr0 = 0; d->raddr1 = 4; d->eval(); CHECK_EQ((int)d->bank_conflict, 1);
  // different banks
  d->raddr0 = 0; d->raddr1 = 1; d->eval(); CHECK_EQ((int)d->bank_conflict, 0);
  // same reg -> no conflict (can broadcast)
  d->raddr0 = 5; d->raddr1 = 5; d->eval(); CHECK_EQ((int)d->bank_conflict, 0);
  // one read disabled -> no conflict
  d->ren1 = 0; d->raddr0 = 0; d->raddr1 = 4; d->eval(); CHECK_EQ((int)d->bank_conflict, 0);
}

TEST(regfile_random) {
  Sim<Vregfile> sim; auto* d = sim.dut;
  uint32_t gold[NV][WS];
  std::memset(gold, 0, sizeof(gold));
  d->we = 0; d->ren0 = 0; d->ren1 = 0; d->wmask = 0;
  d->waddr = 0; d->raddr0 = 0; d->raddr1 = 0;
  for (int l = 0; l < WS; ++l) d->wdata[l] = 0;
  sim.reset();

  std::mt19937 rng(0x9E3779B9u);
  for (int it = 0; it < 5000; ++it) {
    bool we = (rng() & 3) != 0;
    uint8_t waddr = rng() % NV;
    uint32_t wmask = rng();
    uint32_t wd[WS];
    for (int l = 0; l < WS; ++l) wd[l] = rng();
    bool ren0 = rng() & 1, ren1 = rng() & 1;
    uint8_t ra0 = rng() % NV, ra1 = rng() % NV;

    d->we = we; d->waddr = waddr; d->wmask = wmask;
    for (int l = 0; l < WS; ++l) d->wdata[l] = wd[l];
    d->ren0 = ren0; d->raddr0 = ra0; d->ren1 = ren1; d->raddr1 = ra1;

    uint32_t exp0[WS], exp1[WS];
    for (int l = 0; l < WS; ++l) { exp0[l] = gold[ra0][l]; exp1[l] = gold[ra1][l]; }
    bool exp_conf = ren0 && ren1 && ((ra0 % NB) == (ra1 % NB)) && (ra0 != ra1);
    if (we) for (int l = 0; l < WS; ++l) if ((wmask >> l) & 1) gold[waddr][l] = wd[l];

    sim.tick();

    CHECK_EQ((int)d->bank_conflict, (int)exp_conf);
    if (ren0) for (int l = 0; l < WS; ++l) CHECK_EQ(d->rdata0[l], exp0[l]);
    if (ren1) for (int l = 0; l < WS; ++l) CHECK_EQ(d->rdata1[l], exp1[l]);
  }
}

int main(int, char**) { return tf::run_all(); }
