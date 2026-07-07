// gpu_tb.hpp — shared testbench helper for the Sirion GPU top level (M17).
//
// After M17 the CU has no host-facing memory ports of its own (global memory sits behind
// L1 -> L2 inside gpu_top), so ALL kernel-level tests drive Vgpu_top through this helper:
// preload program/constants/memory, launch a grid with one pulse, wait for grid_done
// (which guarantees flushed, host-coherent memory), then read back memory and any CU's
// register file.
#pragma once
#include "Vgpu_top.h"
#include "../rtl_driver.hpp"
#include "test_framework.hpp"
#include "../iss/gpubin.hpp"
#include <cstdint>

namespace sirion {

struct Gpu {
  Sim<Vgpu_top> sim;
  Vgpu_top* d;
  Gpu() : d(sim.dut) {
    d->imem_we = 0; d->cbank_we = 0; d->gmem_we = 0; d->launch = 0;
    d->dbg_addr = 0; d->dbg_warp = 0; d->dbg_cu = 0; d->gdbg_addr = 0;
    d->grid_nx = 0; d->tpb = 0; d->entry = 0;
    sim.reset();
  }
  void load_program(const GpuBin& bin) {
    for (size_t i = 0; i < bin.code.size(); ++i) {
      d->imem_we = 1; d->imem_waddr = (uint32_t)i; d->imem_wdata = bin.code[i]; sim.tick();
    }
    d->imem_we = 0;
    d->entry = bin.entry;
  }
  void set_const(uint32_t idx, uint32_t v) {
    d->cbank_we = 1; d->cbank_waddr = idx; d->cbank_wdata = v; sim.tick(); d->cbank_we = 0;
  }
  void store_gmem(uint32_t byte, uint32_t v) {
    d->gmem_we = 1; d->gmem_waddr = byte >> 2; d->gmem_wdata = v; sim.tick(); d->gmem_we = 0;
  }
  uint32_t load_gmem(uint32_t byte) {
    d->gdbg_addr = byte >> 2; d->eval(); return d->gdbg_data;
  }
  // ONE launch for the whole grid; returns wall-clock cycles from launch to grid_done.
  uint32_t launch_grid(uint32_t grid, uint32_t tpb) {
    uint32_t blocks0 = d->perf_blocks;
    d->grid_nx = grid; d->tpb = tpb;
    d->launch = 1; sim.tick(); d->launch = 0;
    uint32_t cyc = 0;
    while (!d->grid_done && cyc < 16000000) { sim.tick(); ++cyc; }
    CHECK(d->grid_done);
    CHECK(!d->grid_busy);
    CHECK_EQ(d->perf_blocks - blocks0, grid);
    return cyc;
  }
  // registered debug read of one register (all lanes) from a CU's register file
  void read_reg(uint32_t cu, uint32_t warp, uint32_t r) {
    d->dbg_cu = cu; d->dbg_warp = warp; d->dbg_addr = r; sim.tick();
  }
};

} // namespace sirion
