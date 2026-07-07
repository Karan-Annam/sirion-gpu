// test_l1_dcache.cpp — self-checking testbench for rtl/cache/l1_dcache.sv (M14/M17).
//
// The L1 is write-through / no-write-allocate (M17): the TB plays the L2's role on the
// level-handshake memory side (variable ack latency), with a flat word memory as golden.
// Checks: loads always return flat-memory contents (transparency), stores are visible in
// the backing after ack, atomics forward + self-invalidate, flush invalidates.
#include "Vl1_dcache.h"
#include "../rtl_driver.hpp"
#include "../iss/isa.hpp"
#include "test_framework.hpp"
#include <vector>
#include <random>
#include <cstring>

using namespace sirion;

static const int LINE_WORDS = 8;
static const int MEM_WORDS  = 4096;

namespace {

struct CacheTb {
  Sim<Vl1_dcache> sim;
  Vl1_dcache* d;
  std::vector<uint32_t> back;   // backing memory (the "L2"), golden by construction
  std::mt19937 rng{42};
  int ack_delay = 0;            // cycles until we ack the current mem request

  CacheTb() : d(sim.dut), back(MEM_WORDS, 0) {
    d->req_valid = 0; d->req_we = 0; d->req_addr = 0; d->flush = 0;
    d->req_atom = 0; d->req_afunc = 0; d->req_aword = 0; d->req_acmp = 0;
    std::memset(&d->req_wline, 0, sizeof(d->req_wline));
    d->req_wstrb = 0; d->mem_ack = 0;
    sim.reset();
  }
  // service the memory side with a random 1..3 cycle latency
  void tick() {
    d->mem_ack = 0;
    if (d->mem_req) {
      if (ack_delay == 0) ack_delay = 1 + (int)(rng() % 3);
      if (--ack_delay == 0) {
        uint32_t base = (d->mem_addr >> 2) % MEM_WORDS;
        if (d->mem_atom) {
          uint32_t off = (d->mem_addr >> 2) & (LINE_WORDS - 1);
          uint32_t old = back[(base & ~(LINE_WORDS - 1)) + off];
          d->mem_old = old;
          back[(base & ~(LINE_WORDS - 1)) + off] =
            atom_ref(d->mem_afunc, old, d->mem_aword, d->mem_acmp);
        } else if (d->mem_we) {
          for (int w = 0; w < LINE_WORDS; w++)
            if (d->mem_wstrb & (1u << w)) back[base + w] = d->mem_wline[w];
        } else {
          for (int w = 0; w < LINE_WORDS; w++) d->mem_rline[w] = back[base + w];
        }
        d->mem_ack = 1;
      }
    } else ack_delay = 0;
    sim.tick();
    d->mem_ack = 0;
  }
  void access(uint32_t addr, bool we, const uint32_t* wline, uint32_t wstrb,
              uint32_t* out, bool atom = false, uint32_t afunc = 0,
              uint32_t aword = 0, uint32_t acmp = 0, uint32_t* old_out = nullptr) {
    d->req_addr = addr; d->req_we = we; d->req_atom = atom;
    d->req_afunc = afunc; d->req_aword = aword; d->req_acmp = acmp;
    if (we) { for (int w = 0; w < LINE_WORDS; w++) d->req_wline[w] = wline[w]; d->req_wstrb = wstrb; }
    d->req_valid = 1; tick(); d->req_valid = 0;
    int k = 0; while (!d->resp_ready && k < 200) { tick(); ++k; }
    CHECK(d->resp_ready);
    if (out)     for (int w = 0; w < LINE_WORDS; w++) out[w] = d->resp_rline[w];
    if (old_out) *old_out = d->resp_old;
    tick();
  }
  void flush() {
    d->flush = 1; tick(); d->flush = 0;
    int k = 0; while (!d->flush_done && k < 10000) { tick(); ++k; }
    CHECK(d->flush_done);
    tick();
  }
};

} // namespace

TEST(l1d_directed) {
  CacheTb tb;
  uint32_t wl[LINE_WORDS], rd[LINE_WORDS];
  // write-through: a store lands in the backing immediately (after ack)
  for (int w = 0; w < LINE_WORDS; w++) wl[w] = 0xA0 + w;
  tb.access(0x100, true, wl, 0xFF, nullptr);
  for (int w = 0; w < LINE_WORDS; w++) CHECK_EQ(tb.back[(0x100 >> 2) + w], wl[w]);
  // read allocates and returns the line
  tb.access(0x100, false, nullptr, 0, rd);
  for (int w = 0; w < LINE_WORDS; w++) CHECK_EQ(rd[w], wl[w]);
  // partial-strobe store updates both the present line and the backing
  for (int w = 0; w < LINE_WORDS; w++) wl[w] = 0xB0 + w;
  tb.access(0x100, true, wl, (1u << 2) | (1u << 5), nullptr);
  tb.access(0x100, false, nullptr, 0, rd);
  CHECK_EQ(rd[2], 0xB2u); CHECK_EQ(rd[5], 0xB5u); CHECK_EQ(rd[0], 0xA0u);
  CHECK_EQ(tb.back[(0x100 >> 2) + 2], 0xB2u);
  // atomic: forwarded to backing, returns old, self-invalidates the line
  uint32_t old = 0;
  tb.access(0x108, false, nullptr, 0, nullptr, true, ATOM_ADD, 7, 0, &old);
  CHECK_EQ(old, 0xB2u);                       // word 2 of the line at 0x100
  CHECK_EQ(tb.back[(0x100 >> 2) + 2], 0xB2u + 7);
  tb.access(0x100, false, nullptr, 0, rd);    // re-fetch sees the atomic's result
  CHECK_EQ(rd[2], 0xB2u + 7);
}

TEST(l1d_random_transparency) {
  CacheTb tb;
  std::mt19937 rng(1234);
  for (int it = 0; it < 3000; ++it) {
    uint32_t line = (rng() % (MEM_WORDS / LINE_WORDS));
    uint32_t addr = line * LINE_WORDS * 4 + (rng() % LINE_WORDS) * 4;
    uint32_t base = (addr >> 2) & ~(LINE_WORDS - 1);
    int op = rng() % 100;
    if (op < 5) { tb.flush(); continue; }
    if (op < 40) {                                   // strobed store
      uint32_t wl[LINE_WORDS]; uint32_t strb = rng() & 0xFF;
      for (int w = 0; w < LINE_WORDS; w++) wl[w] = rng();
      tb.access(addr, true, wl, strb, nullptr);
      for (int w = 0; w < LINE_WORDS; w++)           // write-through: backing updated NOW
        if (strb & (1u << w)) CHECK_EQ(tb.back[base + w], wl[w]);
    } else if (op < 50) {                            // atomic add
      uint32_t before = tb.back[addr >> 2], old = 0;
      tb.access(addr, false, nullptr, 0, nullptr, true, ATOM_ADD, 3, 0, &old);
      CHECK_EQ(old, before);
      CHECK_EQ(tb.back[addr >> 2], before + 3);
    } else {                                         // load: must equal the backing
      uint32_t rd[LINE_WORDS];
      tb.access(addr, false, nullptr, 0, rd);
      for (int w = 0; w < LINE_WORDS; w++) CHECK_EQ(rd[w], tb.back[base + w]);
    }
  }
  std::printf("  [perf] hits=%u misses=%u\n", (unsigned)tb.d->perf_hits, (unsigned)tb.d->perf_misses);
}

int main(int, char**) { return tf::run_all(); }
