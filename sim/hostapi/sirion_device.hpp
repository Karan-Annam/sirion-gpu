// sirion_device.hpp — host driver / API for the full Sirion GPU (M20).
//
// The C++ face of the device — the "driver" — wrapping the Verilated
// gpu_gfx_top: program upload (multiple programs resident at different entry points),
// buffer upload/readback, COMPUTE dispatches, and hardware DRAW CALLS executed end-to-end
// by the on-chip graphics sequencer (VS on the CUs -> raster -> FS on the CUs -> ROP).
// One object, one command stream — compute and graphics interleave freely.
#pragma once
#include "../rtl_driver.hpp"
#include "../iss/gpubin.hpp"
#include "../gfx/gfx.hpp"
#include <cstdint>
#include <string>

namespace sirion {

struct DrawDesc {
  uint32_t nverts = 0, ntris = 0;
  uint32_t vout = 0, ibuf = 0, fbuf = 0, tex = 0;   // byte addresses in global memory
  uint32_t vs_entry = 0, fs_entry = 0;
  bool     blend = false;
  uint8_t  alpha = 255;
  bool     depth_write = true;
};

template <class DUT>
struct SirionDevice {
  Sim<DUT> sim;
  DUT* d;

  SirionDevice() : d(sim.dut) {
    d->imem_we=0; d->cbank_we=0; d->gmem_we=0; d->launch=0; d->draw=0;
    d->dbg_addr=0; d->dbg_warp=0; d->dbg_cu=0; d->gdbg_addr=0;
    d->grid_nx=0; d->tpb=0; d->entry=0;
    d->r_clear=0; d->r_tri_valid=0; d->r_tex_we=0; d->r_textured=0;
    d->r_tex_bilinear=0; d->r_tex_mip=0; d->r_blend_en=0; d->r_alpha=255; d->r_depth_write=1;
    d->r_emit_mode=0; d->r_fin_valid=0; d->r_fb_raddr=0; d->r_fdbg_idx=0;
    d->r_q0=0x10000; d->r_q1=0x10000; d->r_q2=0x10000;
    sim.reset();
  }

  // load a program at an instruction-word offset; returns its launch entry
  uint32_t loadProgram(const GpuBin& bin, uint32_t word_off = 0) {
    for (size_t i = 0; i < bin.code.size(); ++i) {
      d->imem_we=1; d->imem_waddr=word_off+(uint32_t)i; d->imem_wdata=bin.code[i]; sim.tick();
    }
    d->imem_we=0;
    return word_off + bin.entry;
  }
  void setConst(uint32_t idx, uint32_t v) {
    d->cbank_we=1; d->cbank_waddr=idx; d->cbank_wdata=v; sim.tick(); d->cbank_we=0;
  }
  void memWrite(uint32_t byte, uint32_t v) {
    d->gmem_we=1; d->gmem_waddr=byte>>2; d->gmem_wdata=v; sim.tick(); d->gmem_we=0;
  }
  uint32_t memRead(uint32_t byte) { d->gdbg_addr=byte>>2; d->eval(); return d->gdbg_data; }

  // compute dispatch (returns wall cycles)
  uint32_t dispatch(uint32_t grid, uint32_t tpb, uint32_t entry) {
    d->grid_nx=grid; d->tpb=tpb; d->entry=entry;
    d->launch=1; sim.tick(); d->launch=0;
    uint32_t c=0; while (!d->grid_done && c < 16000000) { sim.tick(); ++c; }
    return c;
  }

  // clear the render target
  void clear(uint32_t rgb) {
    d->r_clear=1; d->r_clear_color=rgb; sim.tick(); d->r_clear=0;
    int c=0; while (d->r_busy && c < 200000) { sim.tick(); ++c; }
  }

  // hardware draw call: the on-chip sequencer runs VS -> raster -> FS -> ROP (wall cycles)
  uint32_t draw(const DrawDesc& ds) {
    d->ds_nverts=ds.nverts; d->ds_ntris=ds.ntris;
    d->ds_vout=ds.vout; d->ds_ibuf=ds.ibuf; d->ds_fbuf=ds.fbuf; d->ds_tex=ds.tex;
    d->ds_vs_entry=ds.vs_entry; d->ds_fs_entry=ds.fs_entry;
    d->ds_blend=ds.blend; d->ds_alpha=ds.alpha; d->ds_dwrite=ds.depth_write;
    d->draw=1; sim.tick(); d->draw=0;
    uint32_t c=0; while (!d->draw_done && c < 16000000) { sim.tick(); ++c; }
    return c;
  }

  void readFramebuffer(gfx::Framebuffer& fb, int W, int H, uint32_t bg) {
    fb.init(W, H, (int32_t)bg);
    for (int i = 0; i < W*H; ++i) {
      d->r_fb_raddr=i; d->eval();
      uint32_t c = d->r_fb_rcolor;
      fb.color[3*i+0]=(c>>16)&0xFF; fb.color[3*i+1]=(c>>8)&0xFF; fb.color[3*i+2]=c&0xFF;
    }
  }
  bool present(const std::string& ppm, int W, int H, uint32_t bg) {
    gfx::Framebuffer fb; readFramebuffer(fb, W, H, bg);
    return gfx::write_ppm(fb, ppm);
  }
};

} // namespace sirion
