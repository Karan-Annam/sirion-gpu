// test_sync_fifo.cpp — self-checking testbench for rtl/common/sync_fifo.sv (M2).
//
// Golden model: std::deque. Random push/pop with capacity checks; verifies dout (head),
// full/empty/count against the model over many cycles.
#include "Vsync_fifo.h"
#include "../rtl_driver.hpp"
#include "test_framework.hpp"
#include <deque>
#include <random>

static constexpr int DEPTH = 8;

TEST(fifo_directed) {
  Sim<Vsync_fifo> sim; auto* d = sim.dut;
  d->push = 0; d->pop = 0; d->din = 0;
  sim.reset();
  CHECK_EQ((int)d->empty, 1);
  CHECK_EQ((int)d->full, 0);
  CHECK_EQ((int)d->count, 0);

  // fill to full
  for (int i = 0; i < DEPTH; ++i) {
    d->push = 1; d->din = 0x100 + i; sim.tick();
    CHECK_EQ((int)d->count, i + 1);
  }
  d->push = 0;
  CHECK_EQ((int)d->full, 1);

  // drain in FIFO order
  for (int i = 0; i < DEPTH; ++i) {
    CHECK_EQ(d->dout, (uint32_t)(0x100 + i));   // head
    d->pop = 1; sim.tick();
  }
  d->pop = 0;
  CHECK_EQ((int)d->empty, 1);
}

TEST(fifo_random) {
  Sim<Vsync_fifo> sim; auto* d = sim.dut;
  std::deque<uint32_t> gold;
  d->push = 0; d->pop = 0; d->din = 0;
  sim.reset();

  std::mt19937 rng(0xF1F0u);
  uint32_t next = 1;
  for (int it = 0; it < 8000; ++it) {
    bool wpush = rng() & 1, wpop = rng() & 1;
    uint32_t din = next;
    bool full = gold.size() == DEPTH, empty = gold.empty();
    bool do_push = wpush && !full, do_pop = wpop && !empty;

    d->push = wpush; d->pop = wpop; d->din = din;
    d->eval();
    CHECK_EQ((int)d->full, (int)full);
    CHECK_EQ((int)d->empty, (int)empty);
    CHECK_EQ((int)d->count, (int)gold.size());
    if (!empty) CHECK_EQ(d->dout, gold.front());   // head is valid when not empty

    // apply model (pop reads front first, then push appends)
    if (do_pop) gold.pop_front();
    if (do_push) { gold.push_back(din); ++next; }

    sim.tick();
  }
}

int main(int, char**) { return tf::run_all(); }
