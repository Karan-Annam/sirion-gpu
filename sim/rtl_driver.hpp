// rtl_driver.hpp — thin Verilator DUT wrapper (clock/reset/trace helpers).
//
// Templated over the Verilated top class. Assumes the DUT exposes top-level ports
// named `clk` and `rst` (synchronous active-high reset — the project convention) and
// the usual Verilator `eval()` / `trace()` methods.
//
// Each Sim owns its own VerilatedContext AND the DUT, so independent tests in one
// executable never collide in the global context (which otherwise breaks tracing and
// model re-registration across tests). Usage:
//     Sim<Vcounter> sim;         // constructs the model on a private context
//     auto* dut = sim.dut;
//     sim.reset(); sim.tick(); ...
#pragma once
#include <verilated.h>
#include <verilated_vcd_c.h>
#include <cstdint>
#include <string>

// Verilator's legacy trace path references sc_time_stamp(); our trace time comes from
// dump(tstamp) instead, so a zero stub satisfies the linker. `used` forces the symbol to
// be emitted even though our TB never calls it; `inline` keeps it ODR-safe across TUs.
__attribute__((used)) inline double sc_time_stamp() { return 0.0; }

template <class DUT>
struct Sim {
  VerilatedContext ctx;
  DUT* dut;
  VerilatedVcdC* tfp = nullptr;
  vluint64_t tstamp = 0;   // trace time (2 units per cycle: one per phase)
  long cycles = 0;

  Sim() { dut = new DUT(&ctx); }
  ~Sim() { close_trace(); delete dut; }

  Sim(const Sim&) = delete;
  Sim& operator=(const Sim&) = delete;

  void open_trace(const std::string& path, int depth = 99) {
    ctx.traceEverOn(true);
    tfp = new VerilatedVcdC;
    dut->trace(tfp, depth);
    tfp->open(path.c_str());
  }
  void close_trace() {
    if (tfp) { tfp->close(); delete tfp; tfp = nullptr; }
  }

  void eval() {
    dut->eval();
    if (tfp) tfp->dump(tstamp);
    ++tstamp;
  }

  // One clock: evaluate the low phase, then the rising edge.
  void tick() {
    dut->clk = 0; eval();
    dut->clk = 1; eval();
    ++cycles;
  }

  void reset(int n = 4) {
    dut->rst = 1;
    for (int i = 0; i < n; ++i) tick();
    dut->rst = 0;
  }
};
