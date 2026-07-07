// test_gfx_pipeline.cpp — M20: the FULL programmable pipeline in hardware, one command
// stream driving compute AND graphics.
//
//   1. COMPUTE dispatch: gen_checker.k (compiled from the C-LIKE LANGUAGE) generates the
//      texture into global memory — the GPU makes its own texture.
//   2. Hardware DRAW CALL (opaque):     the on-chip sequencer (gfx_seq) launches the VS
//      grid, fetches indices+vertices, drives the rasterizer per triangle, DMAs the
//      fragments, sets the FS's kernel parameters, launches the FS grid, and streams the
//      shaded fragments through the ROP — zero host involvement between draw and done.
//   3. Second DRAW CALL (translucent, alpha=160, no depth write) blends on top.
//
// Verification: the final framebuffer must match a stage-exact golden model PIXEL-EXACT
// (golden VS from the shared fp_*_ref functions, golden emit mirroring the raster scan,
// golden FS mirroring the kernel's integer ops, golden ROP in draw order). The rendered
// image is written out.
#include "Vgpu_gfx_top.h"
#include "../hostapi/sirion_device.hpp"
#include "../iss/isa.hpp"
#include "test_framework.hpp"
#include <cstring>
#include <vector>

using namespace sirion;

static const int RW = 96, RH = 96, TW = 32;
static const uint32_t BG = 0x101020;
static const uint32_t VIN = 0x1000, VOUT = 0x2000, TEXP = 0x3000,
                      IBUF_A = 0x5000, IBUF_B = 0x5100, FBUF = 0x10000;

static uint32_t f2u(float f){ uint32_t u; std::memcpy(&u,&f,4); return u; }

namespace {
struct VtxIn  { float x, y, z; uint32_t rgb; int u, v; };
struct VtxOut { int32_t x, y, z, q; uint32_t rgb; uint32_t uvp; };
struct Frag   { uint32_t w0, w1, w2; };

VtxOut golden_vs(const VtxIn& v, float ff, float cx, float cy, float zoff) {
  uint32_t t  = fp_add_ref(f2u(v.z), f2u(zoff));
  uint32_t rw = sfu_rcp_ref(t);
  uint32_t sx = fp_add_ref(fp_mul_ref(fp_mul_ref(f2u(v.x), f2u(ff)), rw), f2u(cx));
  uint32_t sy = fp_add_ref(fp_mul_ref(fp_mul_ref(f2u(v.y), f2u(ff)), rw), f2u(cy));
  VtxOut o;
  o.x = (int32_t)fp_f2i_ref(sx);
  o.y = (int32_t)fp_f2i_ref(sy);
  o.z = (int32_t)fp_f2i_ref(fp_mul_ref(t,  f2u(256.0f)));
  o.q = (int32_t)fp_f2i_ref(fp_mul_ref(rw, f2u(65536.0f)));
  o.rgb = v.rgb;
  o.uvp = ((uint32_t)(v.v << 8) << 16) | (uint32_t)(v.u << 8);
  return o;
}

void golden_emit(std::vector<Frag>& out, const VtxOut& a, const VtxOut& b, const VtxOut& c) {
  auto edge = [](int64_t ax,int64_t ay,int64_t bx,int64_t by,int64_t px,int64_t py) {
    return (bx-ax)*(py-ay) - (by-ay)*(px-ax);
  };
  int64_t area = edge(a.x,a.y, b.x,b.y, c.x,c.y);
  if (area == 0) return;
  bool pos = area > 0;
  auto clampi = [](int v, int lo, int hi){ return v<lo?lo:v>hi?hi:v; };
  int minx = clampi(std::min({a.x,b.x,c.x}), 0, RW-1), maxx = clampi(std::max({a.x,b.x,c.x}), 0, RW-1);
  int miny = clampi(std::min({a.y,b.y,c.y}), 0, RH-1), maxy = clampi(std::max({a.y,b.y,c.y}), 0, RH-1);
  for (int py = miny; py <= maxy; ++py)
    for (int px = minx; px <= maxx; ++px) {
      int64_t w0 = edge(b.x,b.y, c.x,c.y, px,py);
      int64_t w1 = edge(c.x,c.y, a.x,a.y, px,py);
      int64_t w2 = edge(a.x,a.y, b.x,b.y, px,py);
      bool cov = pos ? (w0>=0 && w1>=0 && w2>=0) : (w0<=0 && w1<=0 && w2<=0);
      if (!cov) continue;
      int64_t pw = w0*a.q + w1*b.q + w2*c.q;
      if (pw == 0) continue;
      auto pin = [&](int64_t a0,int64_t a1,int64_t a2) {
        return (int32_t)((w0*a0*a.q + w1*a1*b.q + w2*a2*c.q) / pw);
      };
      int32_t z  = (int32_t)((w0*a.z + w1*b.z + w2*c.z) / area);
      int32_t ui = pin((int32_t)(a.uvp&0xFFFF), (int32_t)(b.uvp&0xFFFF), (int32_t)(c.uvp&0xFFFF));
      int32_t vi = pin((int32_t)(a.uvp>>16),    (int32_t)(b.uvp>>16),    (int32_t)(c.uvp>>16));
      int32_t ri = pin((a.rgb>>16)&0xFF, (b.rgb>>16)&0xFF, (c.rgb>>16)&0xFF);
      int32_t gi = pin((a.rgb>>8)&0xFF,  (b.rgb>>8)&0xFF,  (c.rgb>>8)&0xFF);
      int32_t bi = pin(a.rgb&0xFF,       b.rgb&0xFF,       c.rgb&0xFF);
      Frag f;
      f.w0 = ((uint32_t)(z & 0xFFFF) << 16) | (uint32_t)(py*RW + px);
      f.w1 = ((uint32_t)(vi & 0xFFFF) << 16) | (uint32_t)(ui & 0xFFFF);
      f.w2 = ((uint32_t)(ri & 0xFF) << 16) | ((uint32_t)(gi & 0xFF) << 8) | (uint32_t)(bi & 0xFF);
      out.push_back(f);
    }
}

uint32_t golden_tex(uint32_t i) {                        // mirrors gen_checker.k exactly
  uint32_t x = i & 31, y = i >> 5;
  return (((x >> 2) + (y >> 2)) & 1) ? 15786080u : 3166368u;
}

uint32_t golden_fs(uint32_t w1, uint32_t w2) {           // mirrors fs_texmod.s exactly
  uint32_t tx = ((w1 << 16) >> 24), ty = (w1 >> 24);
  if (tx > 31) tx = 31;
  if (ty > 31) ty = 31;
  uint32_t texel = golden_tex((ty << 5) + tx);
  uint32_t tr = (texel << 8) >> 24, vr = (w2 << 8) >> 24;
  uint32_t tg = (texel << 16) >> 24, vg = (w2 << 16) >> 24;
  uint32_t tb = texel & 255, vb = w2 & 255;
  return (((tr*vr)>>8) << 16) | (((tg*vg)>>8) << 8) | ((tb*vb)>>8);
}
} // namespace

TEST(gfx_pipeline_hw_draw) {
  GpuBin vs, fs, gen; std::string err;
  if (!load_gpubin("build/vs_persp.gpubin", vs, &err)) throw tf::AssertFail("vs: " + err);
  if (!load_gpubin("build/fs_texmod.gpubin", fs, &err)) throw tf::AssertFail("fs: " + err);
  if (!load_gpubin("build/hl_gen_checker.gpubin", gen, &err)) throw tf::AssertFail("gen: " + err);

  SirionDevice<Vgpu_gfx_top> dev;
  uint32_t VS_E  = dev.loadProgram(vs, 0);
  uint32_t FS_E  = dev.loadProgram(fs, 64);
  uint32_t GEN_E = dev.loadProgram(gen, 128);

  // ============ 1) COMPUTE: the GPU generates its own texture (from .k source) ============
  dev.setConst(0, TEXP); dev.setConst(1, TW*TW);
  uint32_t ccyc = dev.dispatch(4, 256, GEN_E);
  for (int i = 0; i < TW*TW; ++i) CHECK_EQ(dev.memRead(TEXP + 4*i), golden_tex(i));
  std::printf("  [frame] texture generated by compute kernel in %u cycles\n", ccyc);

  // ============ 2) scene upload ============
  const float FF = 120.0f, CX = 48.0f, CY = 48.0f, ZOFF = 3.0f;
  std::vector<VtxIn> verts = {
    {-1.2f, -0.5f, 0.2f, 0xFFFFFF,  0, 32}, { 1.2f, -0.5f, 0.2f, 0xFFFFFF, 32, 32},
    { 1.2f,  0.9f, 3.0f, 0xFFFFFF, 32,  0}, {-1.2f,  0.9f, 3.0f, 0xFFFFFF,  0,  0},
    {-0.30f, -0.40f, 0.0f, 0x80A0FF, 0, 0}, { 0.42f, -0.40f, 0.0f, 0x80A0FF, 32, 0},
    { 0.42f,  0.30f, 0.0f, 0x80A0FF, 32, 32}, {-0.30f,  0.30f, 0.0f, 0x80A0FF, 0, 32},
  };
  const uint32_t NV = (uint32_t)verts.size();
  for (uint32_t i = 0; i < NV; ++i) {
    uint32_t b = VIN + i*24;
    dev.memWrite(b,    f2u(verts[i].x)); dev.memWrite(b+4,  f2u(verts[i].y));
    dev.memWrite(b+8,  f2u(verts[i].z)); dev.memWrite(b+12, verts[i].rgb);
    dev.memWrite(b+16, (uint32_t)(verts[i].u << 8)); dev.memWrite(b+20, (uint32_t)(verts[i].v << 8));
  }
  uint32_t idxA[6] = {0,1,2, 0,2,3}, idxB[6] = {4,5,6, 4,6,7};
  for (int k = 0; k < 6; ++k) { dev.memWrite(IBUF_A + 4*k, idxA[k]); dev.memWrite(IBUF_B + 4*k, idxB[k]); }

  auto set_vs_consts = [&] {
    dev.setConst(0, VIN); dev.setConst(1, VOUT); dev.setConst(2, NV);
    dev.setConst(3, f2u(FF)); dev.setConst(4, f2u(CX)); dev.setConst(5, f2u(CY));
    dev.setConst(6, f2u(ZOFF)); dev.setConst(7, f2u(65536.0f)); dev.setConst(8, f2u(256.0f));
  };

  // ============ 3) two hardware draw calls ============
  dev.clear(BG);
  DrawDesc dA; dA.nverts=NV; dA.ntris=2; dA.vout=VOUT; dA.ibuf=IBUF_A; dA.fbuf=FBUF;
  dA.tex=TEXP; dA.vs_entry=VS_E; dA.fs_entry=FS_E;
  set_vs_consts();
  uint32_t cycA = dev.draw(dA);

  DrawDesc dB = dA; dB.ibuf=IBUF_B; dB.blend=true; dB.alpha=160; dB.depth_write=false;
  set_vs_consts();                       // draw A's FS pass overwrote c[0..2]
  uint32_t cycB = dev.draw(dB);
  std::printf("  [frame] draw A: %u cycles, draw B: %u cycles (VS+raster+FS+ROP in hardware)\n",
              cycA, cycB);

  // ============ 4) stage-exact golden model of the whole frame ============
  std::vector<VtxOut> gv(NV);
  for (uint32_t i = 0; i < NV; ++i) gv[i] = golden_vs(verts[i], FF, CX, CY, ZOFF);
  gfx::Framebuffer gold; gold.init(RW, RH, BG);
  auto golden_draw = [&](const uint32_t* idx, bool blend, int alpha, bool dw) {
    std::vector<Frag> frags;
    golden_emit(frags, gv[idx[0]], gv[idx[1]], gv[idx[2]]);
    golden_emit(frags, gv[idx[3]], gv[idx[4]], gv[idx[5]]);
    for (auto& f : frags) {
      uint32_t pidx = f.w0 & 0xFFFF;
      int32_t  z    = (int32_t)(f.w0 >> 16);
      int x = pidx % RW, y = pidx / RW;
      size_t di = (size_t)y*RW + x;
      if (z < gold.depth[di]) {
        uint32_t c = golden_fs(f.w1, f.w2);
        int r=(c>>16)&0xFF, g=(c>>8)&0xFF, b=c&0xFF;
        if (blend) {
          size_t ci = di*3;
          r = (r*alpha + gold.color[ci+0]*(255-alpha)) / 255;
          g = (g*alpha + gold.color[ci+1]*(255-alpha)) / 255;
          b = (b*alpha + gold.color[ci+2]*(255-alpha)) / 255;
        }
        gold.put(x, y, r, g, b);
        if (dw) gold.depth[di] = z;
      }
    }
  };
  golden_draw(idxA, false, 255, true);
  golden_draw(idxB, true,  160, false);

  // ============ 5) pixel-exact framebuffer ============
  gfx::Framebuffer rtl_fb;
  dev.readFramebuffer(rtl_fb, RW, RH, BG);
  int mism = 0;
  for (size_t i = 0; i < rtl_fb.color.size(); ++i)
    if (rtl_fb.color[i] != gold.color[i]) ++mism;
  CHECK_EQ(mism, 0);
  gfx::write_ppm(rtl_fb, "build/render_hw_pipeline.ppm");
  std::printf("  wrote build/render_hw_pipeline.ppm (full pipeline sequenced in hardware)\n");
}

int main(int, char**) { return tf::run_all(); }
