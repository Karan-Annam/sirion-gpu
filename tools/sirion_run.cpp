// sirion_run.cpp — run a compiled Sirion kernel (.gpubin) from the command line.
//
// Two builds from this one source (see Makefile targets `runner` and `runner-rtl`):
//   build/sirion_run.exe      — executes on the golden ISS (instant; great for iterating)
//   build/sirion_run_rtl.exe  — executes on the REAL RTL GPU under Verilator (gpu_top)
//
// Usage:
//   sirion_run <kernel.gpubin> [--grid N] [--tpb N]
//              [--const IDX=VAL]...        kernel parameters (constant bank)
//              [--store ADDR=VAL]...       preload one global-memory word (byte address)
//              [--fill ADDR:COUNT:BASE:STEP]  preload COUNT words: BASE, BASE+STEP, ...
//              [--dump ADDR:COUNT]         print COUNT words of global memory after the run
//              [--dumpf ADDR:COUNT]        same, interpreted as binary32 floats
// VAL accepts decimal, 0x hex, or a float like 1.5f (stored as its bit pattern).
//
// Example (vec_add over 100 elements):
//   sirion_run build/vec_add.gpubin --grid 4 --tpb 32 \
//     --const 0=0x3000 --const 1=0x1000 --const 2=0x2000 --const 3=100 \
//     --fill 0x1000:100:1:1 --fill 0x2000:100:2:3 --dump 0x3000:10
#include "../sim/iss/gpubin.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#ifdef SIRION_RTL
#include "Vgpu_top.h"
#include "../sim/rtl_driver.hpp"
#else
#include "../sim/iss/iss.hpp"
#endif

using namespace sirion;

static uint32_t parse_val(const std::string& s) {
  if (!s.empty() && (s.back() == 'f' || s.back() == 'F') &&
      s.find('.') != std::string::npos) {
    float f = std::stof(s);
    uint32_t u; std::memcpy(&u, &f, 4); return u;
  }
  return (uint32_t)std::stoul(s, nullptr, 0);
}

struct KV { uint32_t a, b, c, d; };

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: sirion_run <kernel.gpubin> [options]\n"); return 2; }
  std::string binpath = argv[1];
  uint32_t grid = 1, tpb = 32;
  std::vector<KV> consts, stores, fills, dumps, dumpfs;

  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
    auto split = [&](const std::string& s, char sep) {
      std::vector<std::string> out; size_t p = 0;
      while (true) { size_t q = s.find(sep, p);
        if (q == std::string::npos) { out.push_back(s.substr(p)); break; }
        out.push_back(s.substr(p, q - p)); p = q + 1; }
      return out;
    };
    if (a == "--grid") grid = parse_val(next());
    else if (a == "--tpb") tpb = parse_val(next());
    else if (a == "--const") { auto p = split(next(), '='); consts.push_back({parse_val(p[0]), parse_val(p[1]), 0, 0}); }
    else if (a == "--store") { auto p = split(next(), '='); stores.push_back({parse_val(p[0]), parse_val(p[1]), 0, 0}); }
    else if (a == "--fill")  { auto p = split(next(), ':');
      fills.push_back({parse_val(p[0]), parse_val(p[1]),
                       p.size() > 2 ? parse_val(p[2]) : 0, p.size() > 3 ? parse_val(p[3]) : 1}); }
    else if (a == "--dump")  { auto p = split(next(), ':'); dumps.push_back({parse_val(p[0]), parse_val(p[1]), 0, 0}); }
    else if (a == "--dumpf") { auto p = split(next(), ':'); dumpfs.push_back({parse_val(p[0]), parse_val(p[1]), 0, 0}); }
    else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }

  GpuBin bin; std::string err;
  if (!load_gpubin(binpath, bin, &err)) { std::fprintf(stderr, "load %s: %s\n", binpath.c_str(), err.c_str()); return 1; }
  std::printf("[sirion_run] %s: %zu instructions, grid=%u tpb=%u  (%s)\n",
              binpath.c_str(), bin.code.size(), grid, tpb,
#ifdef SIRION_RTL
              "RTL GPU under Verilator"
#else
              "golden ISS"
#endif
  );

#ifdef SIRION_RTL
  Sim<Vgpu_top> sim; auto* d = sim.dut;
  d->imem_we=0; d->cbank_we=0; d->gmem_we=0; d->launch=0;
  d->dbg_addr=0; d->dbg_warp=0; d->dbg_cu=0; d->gdbg_addr=0;
  d->grid_nx=0; d->tpb=0; d->entry=0;
  sim.reset();
  for (size_t i = 0; i < bin.code.size(); ++i) {
    d->imem_we=1; d->imem_waddr=(uint32_t)i; d->imem_wdata=bin.code[i]; sim.tick();
  }
  d->imem_we=0; d->entry=bin.entry;
  for (auto& c : consts) { d->cbank_we=1; d->cbank_waddr=c.a; d->cbank_wdata=c.b; sim.tick(); d->cbank_we=0; }
  auto st = [&](uint32_t byte, uint32_t v){ d->gmem_we=1; d->gmem_waddr=byte>>2; d->gmem_wdata=v; sim.tick(); d->gmem_we=0; };
  for (auto& s : stores) st(s.a, s.b);
  for (auto& f : fills) for (uint32_t k = 0; k < f.b; ++k) st(f.a + 4*k, f.c + f.d*k);
  d->grid_nx=grid; d->tpb=tpb;
  d->launch=1; sim.tick(); d->launch=0;
  uint64_t cyc = 0;
  while (!d->grid_done && cyc < 100000000ull) { sim.tick(); ++cyc; }
  if (!d->grid_done) { std::fprintf(stderr, "TIMEOUT after %llu cycles\n", (unsigned long long)cyc); return 1; }
  std::printf("[sirion_run] grid done: %llu cycles, %u insns, %u memops, L1 %u/%u hit/miss, L2 %u/%u, IPC %.3f\n",
              (unsigned long long)cyc, (unsigned)d->perf_insns, (unsigned)d->perf_memops,
              (unsigned)d->perf_l1_hits, (unsigned)d->perf_l1_misses,
              (unsigned)d->perf_l2_hits, (unsigned)d->perf_l2_misses,
              (double)d->perf_insns / (double)(cyc ? cyc : 1));
  auto ld = [&](uint32_t byte){ d->gdbg_addr=byte>>2; d->eval(); return (uint32_t)d->gdbg_data; };
#else
  Iss iss; iss.bin = bin;
  iss.blockDim = {tpb, 1, 1}; iss.gridDim = {grid, 1, 1};
  iss.shared_words = 4096;
  iss.ensure_gmem(1u << 20);
  for (auto& c : consts) iss.set_const(c.a, c.b);
  for (auto& s : stores) iss.gst(s.a, s.b);
  for (auto& f : fills) for (uint32_t k = 0; k < f.b; ++k) iss.gst(f.a + 4*k, f.c + f.d*k);
  iss.run();
  std::printf("[sirion_run] done: %llu warp-instructions, %llu lane-ops, %llu divergent branches\n",
              (unsigned long long)iss.stats.dyn_insns, (unsigned long long)iss.stats.lane_ops,
              (unsigned long long)iss.stats.divergent_branches);
  auto ld = [&](uint32_t byte){ return iss.gld(byte); };
#endif

  for (auto& du : dumps) {
    std::printf("[mem 0x%X]", du.a);
    for (uint32_t k = 0; k < du.b; ++k) std::printf(" %u", ld(du.a + 4*k));
    std::printf("\n");
  }
  for (auto& du : dumpfs) {
    std::printf("[memf 0x%X]", du.a);
    for (uint32_t k = 0; k < du.b; ++k) {
      uint32_t u = ld(du.a + 4*k); float f; std::memcpy(&f, &u, 4);
      std::printf(" %g", f);
    }
    std::printf("\n");
  }
  return 0;
}
