// test_counter.cpp — self-checking testbench for rtl/common/counter.sv (M0).
//
// Purpose: validate the entire flow — Verilator build, C++ harness, self-checking,
// SVA (--assert), and waveform (--trace) generation — on a trivial known-good module.
#include "Vcounter.h"
#include "../rtl_driver.hpp"
#include "test_framework.hpp"

// Basic reset + increment behavior.
TEST(counter_basic_increment) {
  Sim<Vcounter> sim;
  auto* dut = sim.dut;
  dut->en = 0; dut->load = 0; dut->load_val = 0;
  sim.reset();
  CHECK_EQ((int)dut->count, 0);

  dut->en = 1;
  for (int i = 1; i <= 5; ++i) {
    sim.tick();
    CHECK_EQ((int)dut->count, i);
    CHECK_EQ((int)dut->tick, 0);
  }
}

// Enable gating: counter holds when en is low.
TEST(counter_enable_gating) {
  Sim<Vcounter> sim;
  auto* dut = sim.dut;
  dut->en = 0; dut->load = 0; dut->load_val = 0;
  sim.reset();

  dut->en = 1; sim.tick(); sim.tick();   // count == 2
  CHECK_EQ((int)dut->count, 2);
  dut->en = 0;
  for (int i = 0; i < 4; ++i) { sim.tick(); CHECK_EQ((int)dut->count, 2); }
}

// Synchronous load takes priority over enable.
TEST(counter_load_priority) {
  Sim<Vcounter> sim;
  auto* dut = sim.dut;
  dut->en = 1; dut->load = 1; dut->load_val = 100;
  sim.reset();
  dut->load = 1; dut->load_val = 100; dut->en = 1;
  sim.tick();
  CHECK_EQ((int)dut->count, 100);   // load wins over en
}

// Wrap-around produces a 1-cycle tick pulse.
TEST(counter_wrap_tick) {
  Sim<Vcounter> sim;
  auto* dut = sim.dut;
  dut->en = 0; dut->load = 0; dut->load_val = 0;
  sim.reset();

  dut->load = 1; dut->load_val = 253; sim.tick();
  CHECK_EQ((int)dut->count, 253);
  dut->load = 0; dut->en = 1;
  sim.tick(); CHECK_EQ((int)dut->count, 254); CHECK_EQ((int)dut->tick, 0);
  sim.tick(); CHECK_EQ((int)dut->count, 255); CHECK_EQ((int)dut->tick, 0);
  sim.tick(); CHECK_EQ((int)dut->count, 0);   CHECK_EQ((int)dut->tick, 1);  // wrap
  sim.tick(); CHECK_EQ((int)dut->count, 1);   CHECK_EQ((int)dut->tick, 0);
}

// Dedicated waveform run: a rich ~45-cycle scenario (reset -> count -> hold -> load ->
// count through a wrap) dumped to build/counter.vcd. Open it and "zoom fit" to see clk,
// count[7:0], en/load, and the tick pulse. This is the trace 'make waves' opens.
TEST(counter_waveform) {
  Sim<Vcounter> sim;
  auto* dut = sim.dut;
  sim.open_trace("build/counter.vcd");
  dut->en = 0; dut->load = 0; dut->load_val = 0;
  sim.reset(3);

  dut->en = 1;                                   // count 0 -> 16
  for (int i = 0; i < 16; ++i) sim.tick();
  CHECK_EQ((int)dut->count, 16);

  dut->en = 0;                                   // hold for 5 cycles (flat)
  for (int i = 0; i < 5; ++i) sim.tick();
  CHECK_EQ((int)dut->count, 16);

  dut->load = 1; dut->load_val = 250; sim.tick(); dut->load = 0;  // load 250
  CHECK_EQ((int)dut->count, 250);

  dut->en = 1;                                   // 250 -> wrap -> 4, tick pulse at wrap
  for (int i = 0; i < 10; ++i) sim.tick();
  CHECK_EQ((int)dut->count, (250 + 10) & 0xFF);  // == 4

  for (int i = 0; i < 3; ++i) sim.tick();        // trailing idle so the tail is visible
  sim.close_trace();
}

int main(int, char**) {
  return tf::run_all();
}
