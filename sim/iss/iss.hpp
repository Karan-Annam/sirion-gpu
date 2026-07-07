// iss.hpp — Sirion functional instruction-set simulator (the golden reference model).
//
// Executes a .gpubin over a grid of workgroups with full SIMT semantics: per-lane
// registers/predicates, guard predication, and an IPDOM reconvergence stack for
// divergent branches. Functional (not cycle-accurate): warps run to completion serially,
// which is correct for M1 kernels (no cross-warp barriers/atomics yet — those arrive M6).
//
// SIMT reconvergence model (structured control flow; assembler emits `SSY R` before each
// divergent branch to name the reconvergence/immediate-post-dominator point R):
//   * A warp keeps a stack of frames {mask, pc, rpc, pending_rpc}. TOS executes.
//   * `SSY R` records pending_rpc = R on the current frame.
//   * A divergent branch splits `active` into taken/not-taken. The current frame becomes
//     the JOIN frame (mask = full pre-divergence mask, pc = R, keeps its outer rpc); two
//     child frames (rpc = R) are pushed for the two paths. When a child's pc reaches its
//     rpc it simply pops (its lanes are already held by the join frame), so no mask merge
//     is needed and nesting/loops work.
#pragma once
#include "isa.hpp"
#include "gpubin.hpp"
#include <cstdint>
#include <vector>
#include <string>

namespace sirion {

struct Dim3 { uint32_t x = 1, y = 1, z = 1; uint32_t size() const { return x * y * z; } };

struct Stats {
  uint64_t dyn_insns = 0;      // warp-level instructions retired
  uint64_t lane_ops  = 0;      // active-lane operations (dyn_insns weighted by active lanes)
  uint64_t divergent_branches = 0;
  uint64_t warps = 0;
};

class Iss {
public:
  GpuBin bin;
  Dim3 gridDim, blockDim;
  std::vector<uint8_t>  gmem;    // global memory, byte-addressed, little-endian
  std::vector<uint32_t> cbank;   // constant bank, word-indexed (params + static const)
  uint32_t shared_words = 0;     // per-workgroup shared memory (words) for LDS/STS
  Stats stats;

  // Optional final-state capture (for RTL diffing from M3 on): records the final per-lane
  // registers + predicates of one warp (block-linear cap_block, warp cap_warp).
  bool     cap_enable = false;
  uint32_t cap_block = 0, cap_warp = 0;
  uint32_t cap_regs[WARP_SIZE][NUM_VREGS] = {};
  uint32_t cap_pred[NUM_PREDS] = {};
  uint32_t cap_exited = 0;

  // M11 multi-warp capture: final state of EVERY warp in cap_block (index = warp id).
  struct WarpCap {
    uint32_t regs[WARP_SIZE][NUM_VREGS] = {};
    uint32_t pred[NUM_PREDS] = {};
    uint32_t exited = 0;
  };
  std::vector<WarpCap> cap_warps;

  static constexpr uint32_t DONE_PC = 0xFFFFFFFFu;

  void ensure_gmem(size_t bytes) { if (gmem.size() < bytes) gmem.resize(bytes, 0); }
  uint32_t gld(uint32_t a) {
    if ((size_t)a + 4 > gmem.size()) return 0;
    return (uint32_t)gmem[a] | (gmem[a+1] << 8) | (gmem[a+2] << 16) | ((uint32_t)gmem[a+3] << 24);
  }
  void gst(uint32_t a, uint32_t v) {
    ensure_gmem((size_t)a + 4);
    gmem[a] = v & 0xFF; gmem[a+1] = (v >> 8) & 0xFF;
    gmem[a+2] = (v >> 16) & 0xFF; gmem[a+3] = (v >> 24) & 0xFF;
  }

  // Load params/const into the constant bank (host responsibility at launch).
  void set_const(uint32_t idx, uint32_t val) {
    if (cbank.size() <= idx) cbank.resize(idx + 1, 0);
    cbank[idx] = val;
  }

  void run() {
    // Seed the constant bank with any static const words from the file (params, set by
    // the host via set_const(), take precedence at low indices).
    for (size_t i = 0; i < bin.kconst.size(); ++i)
      if (cbank.size() <= i) cbank.push_back(bin.kconst[i]);
    for (uint32_t bz = 0; bz < gridDim.z; ++bz)
    for (uint32_t by = 0; by < gridDim.y; ++by)
    for (uint32_t bx = 0; bx < gridDim.x; ++bx)
      run_block(bx, by, bz);
  }

private:
  struct Frame { uint32_t mask, pc, rpc, pending_rpc; };

  static bool cmp(uint8_t cc, uint32_t a, uint32_t b) {
    switch (cc) {
      case CMP_EQ:  return a == b;
      case CMP_NE:  return a != b;
      case CMP_LT:  return (int32_t)a <  (int32_t)b;
      case CMP_LE:  return (int32_t)a <= (int32_t)b;
      case CMP_GT:  return (int32_t)a >  (int32_t)b;
      case CMP_GE:  return (int32_t)a >= (int32_t)b;
      case CMP_LTU: return a <  b;
      case CMP_GEU: return a >= b;
    }
    return false;
  }

  static bool is_imm_alu(uint8_t op) {
    return op == OP_ADDI || op == OP_ANDI || op == OP_ORI || op == OP_XORI ||
           op == OP_SHLI || op == OP_SHRI || op == OP_SRAI || op == OP_SLTI || op == OP_SLTIU;
  }
  static bool is_alu(uint8_t op) {
    return (op >= OP_ADD && op <= OP_SEQ) || (op >= OP_ADDI && op <= OP_MOV);
  }

  uint32_t sreg(uint8_t sr, uint32_t lane, uint32_t warp_id,
                uint32_t bx, uint32_t by, uint32_t bz, uint32_t tpb) const {
    uint32_t t = warp_id * WARP_SIZE + lane;      // linear thread id within block
    uint32_t bdx = blockDim.x, bdy = blockDim.y;
    uint32_t tid_x = t % bdx;
    uint32_t tid_y = (t / bdx) % bdy;
    uint32_t tid_z = t / (bdx * bdy);
    switch (sr) {
      case SR_TID_X: return tid_x;   case SR_TID_Y: return tid_y;   case SR_TID_Z: return tid_z;
      case SR_NTID_X: return blockDim.x; case SR_NTID_Y: return blockDim.y; case SR_NTID_Z: return blockDim.z;
      case SR_CTAID_X: return bx;    case SR_CTAID_Y: return by;    case SR_CTAID_Z: return bz;
      case SR_NCTAID_X: return gridDim.x; case SR_NCTAID_Y: return gridDim.y; case SR_NCTAID_Z: return gridDim.z;
      case SR_LANEID: return lane;   case SR_WARPID: return warp_id;
      case SR_TID_FLAT: {
        uint32_t block_lin = (bz * gridDim.y + by) * gridDim.x + bx;
        return block_lin * tpb + t;
      }
    }
    return 0;
  }

  // Persistent per-warp state (M11): lets the warps of a block be stepped cooperatively so a
  // real BAR can synchronize them (a warp parks at the barrier until all live warps arrive).
  struct WarpState {
    uint32_t vreg[WARP_SIZE][NUM_VREGS];
    uint32_t pred[NUM_PREDS];
    uint32_t exited;
    std::vector<Frame> st;
    uint32_t warp_id  = 0;
    bool     at_barrier = false;
    bool     finished   = false;
  };

  void run_block(uint32_t bx, uint32_t by, uint32_t bz) {
    const uint32_t tpb = blockDim.size();
    const uint32_t nwarps = (tpb + WARP_SIZE - 1) / WARP_SIZE;
    std::vector<uint32_t> smem(shared_words, 0);   // per-workgroup shared memory (all warps)

    std::vector<WarpState> ws(nwarps);
    for (uint32_t w = 0; w < nwarps; ++w) {
      stats.warps++;
      uint32_t present = 0;                          // lanes whose thread id < tpb
      for (uint32_t l = 0; l < WARP_SIZE; ++l)
        if (w * WARP_SIZE + l < tpb) present |= (1u << l);
      for (auto& row : ws[w].vreg) for (auto& r : row) r = 0;
      ws[w].pred[0] = 0xFFFFFFFFu; ws[w].pred[1] = ws[w].pred[2] = ws[w].pred[3] = 0;
      ws[w].exited = ~present;                       // absent lanes permanently off
      ws[w].st.push_back({present, bin.entry, DONE_PC, DONE_PC});
      ws[w].warp_id = w;
    }

    // Cooperative scheduler: run any ready warp to its next yield (BAR or exit); when no
    // warp is ready but some are parked at a barrier, release the barrier; else the block
    // is done. Mirrors the RTL warp_sched + at_barrier release rule in cu_core.sv, so both
    // models agree for race-free kernels (writes visible across warps only after the BAR).
    for (;;) {
      int pick = -1;
      for (uint32_t w = 0; w < nwarps; ++w)
        if (!ws[w].finished && !ws[w].at_barrier) { pick = (int)w; break; }
      if (pick >= 0) { step_warp(ws[pick], smem, bx, by, bz, tpb); continue; }
      bool any_parked = false;
      for (uint32_t w = 0; w < nwarps; ++w)
        if (!ws[w].finished && ws[w].at_barrier) any_parked = true;
      if (!any_parked) break;                                          // all warps finished
      for (uint32_t w = 0; w < nwarps; ++w) ws[w].at_barrier = false;  // release barrier
    }

    if (cap_enable) {
      uint32_t block_lin = (bz * gridDim.y + by) * gridDim.x + bx;
      if (block_lin == cap_block) {
        cap_warps.assign(nwarps, WarpCap{});
        for (uint32_t w = 0; w < nwarps; ++w) {
          for (uint32_t l = 0; l < WARP_SIZE; ++l)
            for (int r = 0; r < NUM_VREGS; ++r) cap_warps[w].regs[l][r] = ws[w].vreg[l][r];
          for (int p = 0; p < NUM_PREDS; ++p) cap_warps[w].pred[p] = ws[w].pred[p];
          cap_warps[w].exited = ws[w].exited;
        }
        if (cap_warp < nwarps) {                     // legacy single-warp capture
          for (uint32_t l = 0; l < WARP_SIZE; ++l)
            for (int r = 0; r < NUM_VREGS; ++r) cap_regs[l][r] = ws[cap_warp].vreg[l][r];
          for (int p = 0; p < NUM_PREDS; ++p) cap_pred[p] = ws[cap_warp].pred[p];
          cap_exited = ws[cap_warp].exited;
        }
      }
    }
  }

  // Run one warp until it yields: hits BAR (parks at the barrier) or its stack empties.
  void step_warp(WarpState& W, std::vector<uint32_t>& smem,
                 uint32_t bx, uint32_t by, uint32_t bz, uint32_t tpb) {
    auto&          vreg    = W.vreg;
    auto&          pred    = W.pred;
    uint32_t&      exited  = W.exited;
    auto&          st      = W.st;
    const uint32_t warp_id = W.warp_id;

    while (!st.empty()) {
      Frame& top = st.back();
      uint32_t active = top.mask & ~exited;
      if (active == 0)          { st.pop_back(); continue; }
      if (top.pc == top.rpc)    { st.pop_back(); continue; }
      if (top.pc >= bin.code.size()) { exited |= active; st.pop_back(); continue; }

      Insn in(bin.code[top.pc]);
      uint32_t g = (in.psel() == 0) ? 0xFFFFFFFFu : pred[in.psel()];
      if (in.gneg()) g = ~g;
      uint32_t gm = active & g;      // lanes that actually execute (guard-qualified)
      uint8_t op = in.opcode();
      stats.dyn_insns++;
      stats.lane_ops += (uint64_t)__builtin_popcount(gm);

      switch (op) {
        case OP_NOP:  top.pc++; break;
        case OP_BAR:  top.pc++; W.at_barrier = true; return;   // park at barrier, yield

        // ---- floating point (M15): shared fp_*_ref algorithms, bit-exact vs RTL ----
        case OP_FADD: case OP_FSUB: case OP_FMUL: case OP_FFMA:
        case OP_FMIN: case OP_FMAX: case OP_I2F:  case OP_F2I: {
          for (uint32_t l = 0; l < WARP_SIZE; ++l) {
            if (!(gm & (1u << l))) continue;
            uint32_t a = vreg[l][in.rs1()], b = vreg[l][in.rs2()], r = 0;
            switch (op) {
              case OP_FADD: r = fp_add_ref(a, b); break;
              case OP_FSUB: r = fp_sub_ref(a, b); break;
              case OP_FMUL: r = fp_mul_ref(a, b); break;
              case OP_FFMA: r = fp_fma_ref(a, b, vreg[l][in.rd()]); break;
              case OP_FMIN: r = fp_min_ref(a, b); break;
              case OP_FMAX: r = fp_max_ref(a, b); break;
              case OP_I2F:  r = fp_i2f_ref(a); break;
              case OP_F2I:  r = fp_f2i_ref(a); break;
            }
            vreg[l][in.rd()] = r;
          }
          top.pc++; break;
        }
        case OP_FSETP: {
          uint8_t pd = in.pd(), cc = in.cmp();
          uint32_t np = (pd < NUM_PREDS) ? pred[pd] : 0;
          for (uint32_t l = 0; l < WARP_SIZE; ++l) {
            if (!(gm & (1u << l))) continue;
            uint32_t a = vreg[l][in.prs1()], b = vreg[l][in.prs2()];
            if (fp_cmp_ref(cc, a, b)) np |= (1u << l); else np &= ~(1u << l);
          }
          if (pd != PRED_TRUE) pred[pd] = np;
          top.pc++; break;
        }

        // ---- M16: SFU + atomics ----
        case OP_MUFU: {
          uint8_t fn = (uint8_t)(in.raw & 0x7);
          for (uint32_t l = 0; l < WARP_SIZE; ++l)
            if (gm & (1u << l)) vreg[l][in.rd()] = sfu_ref(fn, vreg[l][in.rs1()]);
          top.pc++; break;
        }
        case OP_ATOMG: case OP_ATOMS: {
          // lanes commit in ascending lane order — this DEFINES same-address semantics
          uint8_t fn = (uint8_t)((in.raw >> 8) & 0x7);
          for (uint32_t l = 0; l < WARP_SIZE; ++l) {
            if (!(gm & (1u << l))) continue;
            uint32_t addr = vreg[l][in.rs1()];
            uint32_t val  = vreg[l][in.rs2()];
            uint32_t cmpv = vreg[l][in.rd()];
            uint32_t old;
            if (op == OP_ATOMG) {
              old = gld(addr);
              gst(addr, atom_ref(fn, old, val, cmpv));
            } else {
              uint32_t idx = addr >> 2;
              old = (idx < smem.size()) ? smem[idx] : 0;
              if (idx < smem.size()) smem[idx] = atom_ref(fn, old, val, cmpv);
            }
            vreg[l][in.rd()] = old;
          }
          top.pc++; break;
        }
        case OP_EXIT: exited |= gm; top.pc++; break;
        case OP_SSY:  top.pending_rpc = top.pc + (uint32_t)in.off23(); top.pc++; break;
        case OP_SYNC: top.pc = (top.rpc == DONE_PC) ? (uint32_t)bin.code.size() : top.rpc; break;
        case OP_RET:  top.pc++; break;   // CALL/RET unused in M1
        case OP_CALL: top.pc = top.pc + (uint32_t)in.off23(); break;

        case OP_BRA: {
          uint32_t taken = gm;                 // guard-true lanes branch
          uint32_t ntk   = active & ~taken;
          uint32_t target = top.pc + (uint32_t)in.off23();
          if (taken == 0) { top.pc++; break; }        // nobody branches
          if (ntk == 0)   { top.pc = target; break; } // everybody branches (uniform)
          // divergent branch
          stats.divergent_branches++;
          uint32_t R = (top.pending_rpc != DONE_PC) ? top.pending_rpc : top.rpc;
          uint32_t outer_rpc = top.rpc;
          uint32_t fall = top.pc + 1;
          top.pc = R; top.mask = active; top.rpc = outer_rpc; top.pending_rpc = DONE_PC;
          st.push_back({ntk,   fall,   R, DONE_PC});
          st.push_back({taken, target, R, DONE_PC});
          break;
        }

        case OP_RDSR: {
          uint8_t sr = (uint8_t)in.imm19();
          for (uint32_t l = 0; l < WARP_SIZE; ++l)
            if (gm & (1u << l)) vreg[l][in.rd()] = sreg(sr, l, warp_id, bx, by, bz, tpb);
          top.pc++; break;
        }

        case OP_ISETP: case OP_ISETPI: {
          uint8_t pd = in.pd(), cc = in.cmp();
          uint32_t np = (pd < NUM_PREDS) ? pred[pd] : 0;
          for (uint32_t l = 0; l < WARP_SIZE; ++l) {
            if (!(gm & (1u << l))) continue;
            uint32_t a = vreg[l][in.prs1()];
            uint32_t b = (op == OP_ISETP) ? vreg[l][in.prs2()] : (uint32_t)in.imm14();
            if (cmp(cc, a, b)) np |= (1u << l); else np &= ~(1u << l);
          }
          if (pd != PRED_TRUE) pred[pd] = np;   // P0 is read-only true
          top.pc++; break;
        }

        case OP_LDG: case OP_LDS: case OP_LDC: {
          for (uint32_t l = 0; l < WARP_SIZE; ++l) {
            if (!(gm & (1u << l))) continue;
            uint32_t val;
            if (op == OP_LDC) {
              uint32_t idx = (uint32_t)in.imm19();
              val = (idx < cbank.size()) ? cbank[idx] : 0;
            } else if (op == OP_LDG) {
              val = gld(vreg[l][in.rs1()] + (uint32_t)in.imm15());
            } else { // LDS
              uint32_t idx = (vreg[l][in.rs1()] + (uint32_t)in.imm15()) >> 2;
              val = (idx < smem.size()) ? smem[idx] : 0;
            }
            vreg[l][in.rd()] = val;
          }
          top.pc++; break;
        }

        case OP_STG: case OP_STS: {
          for (uint32_t l = 0; l < WARP_SIZE; ++l) {
            if (!(gm & (1u << l))) continue;
            uint32_t addr = vreg[l][in.rs1()] + (uint32_t)in.imm15();
            uint32_t data = vreg[l][in.rd()];   // rd field carries the store-data reg
            if (op == OP_STG) gst(addr, data);
            else { uint32_t idx = addr >> 2; if (idx < smem.size()) smem[idx] = data; }
          }
          top.pc++; break;
        }

        default: {   // integer ALU (R-type, I-type, MOV, MOVI, NOT)
          for (uint32_t l = 0; l < WARP_SIZE; ++l) {
            if (!(gm & (1u << l))) continue;
            uint32_t a, b = 0;
            if (op == OP_MOVI)      { a = (uint32_t)in.imm19(); }
            else if (op == OP_MOV || op == OP_NOT) { a = vreg[l][in.rs1()]; }
            else if (is_imm_alu(op)) { a = vreg[l][in.rs1()]; b = (uint32_t)in.imm15(); }
            else                     { a = vreg[l][in.rs1()]; b = vreg[l][in.rs2()]; }
            vreg[l][in.rd()] = alu_ref(op, a, b);
          }
          top.pc++; break;
        }
      }
    }

    W.finished = true;   // stack empty: this warp has run to completion
  }
};

} // namespace sirion
