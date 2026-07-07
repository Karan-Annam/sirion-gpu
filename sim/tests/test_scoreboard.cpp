// test_scoreboard.cpp — self-checking testbench for rtl/compute/scoreboard.sv (M2).
//
// Golden model: a 16-bit busy vector. Checks combinational hazard (stall) detection and
// the set-on-issue / clear-on-writeback busy state over directed and random sequences.
#include "Vscoreboard.h"
#include "../rtl_driver.hpp"
#include "test_framework.hpp"
#include <random>

static constexpr int NV = 16;

TEST(scoreboard_directed) {
  Sim<Vscoreboard> sim; auto* d = sim.dut;
  d->check_valid = 0; d->issue = 0; d->wb_valid = 0;
  d->chk_rs1 = 0; d->chk_rs2 = 0; d->chk_rd = 0;
  d->chk_uses_rs1 = 0; d->chk_uses_rs2 = 0; d->chk_writes_rd = 0; d->wb_rd = 0;
  sim.reset();

  // issue a write to R5
  d->check_valid = 1; d->chk_rd = 5; d->chk_writes_rd = 1;
  d->eval();
  CHECK_EQ((int)d->stall, 0);           // R5 not busy yet
  d->issue = 1; sim.tick(); d->issue = 0;
  CHECK_EQ((int)d->busy_vec, 1 << 5);

  // now reading R5 must stall (RAW)
  d->check_valid = 1; d->chk_writes_rd = 0; d->chk_uses_rs1 = 1; d->chk_rs1 = 5;
  d->eval();
  CHECK_EQ((int)d->stall, 1);
  // reading a different reg does not stall
  d->chk_rs1 = 6; d->eval();
  CHECK_EQ((int)d->stall, 0);
  // writing R5 again must stall (WAW)
  d->chk_uses_rs1 = 0; d->chk_writes_rd = 1; d->chk_rd = 5; d->eval();
  CHECK_EQ((int)d->stall, 1);

  // writeback R5 clears it
  d->check_valid = 0; d->wb_valid = 1; d->wb_rd = 5; sim.tick(); d->wb_valid = 0;
  CHECK_EQ((int)d->busy_vec, 0);
  d->check_valid = 1; d->chk_writes_rd = 0; d->chk_uses_rs1 = 1; d->chk_rs1 = 5; d->eval();
  CHECK_EQ((int)d->stall, 0);
}

TEST(scoreboard_random) {
  Sim<Vscoreboard> sim; auto* d = sim.dut;
  uint32_t busy = 0;   // golden
  d->check_valid = 0; d->issue = 0; d->wb_valid = 0;
  sim.reset();

  std::mt19937 rng(0x5C0Eu);
  for (int it = 0; it < 5000; ++it) {
    bool cv   = rng() & 1;
    uint8_t rs1 = rng() % NV, rs2 = rng() % NV, rd = rng() % NV;
    bool u1 = rng() & 1, u2 = rng() & 1, wr = rng() & 1;
    bool wb = (rng() & 3) == 0;
    uint8_t wbrd = rng() % NV;

    bool exp_stall = cv && ((u1 && (busy >> rs1 & 1)) ||
                            (u2 && (busy >> rs2 & 1)) ||
                            (wr && (busy >> rd  & 1)));
    bool iss = cv && !exp_stall && (rng() & 1);

    d->check_valid = cv; d->chk_rs1 = rs1; d->chk_rs2 = rs2; d->chk_rd = rd;
    d->chk_uses_rs1 = u1; d->chk_uses_rs2 = u2; d->chk_writes_rd = wr;
    d->issue = iss; d->wb_valid = wb; d->wb_rd = wbrd;
    d->eval();
    CHECK_EQ((int)d->stall, (int)exp_stall);

    uint32_t nb = busy;
    if (wb) nb &= ~(1u << wbrd);
    if (iss && wr) nb |= (1u << rd);
    busy = nb;

    sim.tick();
    CHECK_EQ((int)d->busy_vec, (int)busy);
  }
}

int main(int, char**) { return tf::run_all(); }
