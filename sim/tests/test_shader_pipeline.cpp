// test_shader_pipeline.cpp — M19: PROGRAMMABLE SHADERS running on the compute unit.
//
// The full programmable flow on one DUT (gpu_gfx_top = compute GPU + rasterizer):
//   1. VERTEX SHADER  = vs_persp.gpubin on the CU (FP transform + MUFU.RCP perspective
//      divide + F2I), model-space vertices -> screen-space vertices in global memory.
//   2. RASTER (emit)  = fixed-function coverage + perspective-correct varying interp,
//      fragment records into the fragment buffer.
//   3. FRAGMENT SHADER = fs_texmod.gpubin on the CU: texture sampled with ORDINARY global
//      loads + color modulation; records shaded in place.
//   4. ROP            = fragments fed back to the rasterizer's fragment port: late depth
//      test + (for the second draw) alpha blending + framebuffer write.
// The host moves the small buffers between engines (M20 automates this).
//
// Verification: every stage is diffed against a stage-exact golden model built from the
// SAME shared reference functions the ISS executes (fp_*_ref / sfu_rcp_ref), and the final
// framebuffer must match PIXEL-EXACT. The image is written out — rendered by shaders
// running on the Sirion GPU.
#include "Vgpu_gfx_top.h"
#include "../rtl_driver.hpp"
#include "../iss/isa.hpp"
#include "../iss/gpubin.hpp"
#include "../gfx/gfx.hpp"
#include "test_framework.hpp"
#include <cstring>
#include <vector>

using namespace sirion;

static const int RW = 96, RH = 96, TW = 32;
static const uint32_t BG = 0x101020;
static const uint32_t VIN = 0x1000, VOUT = 0x2000, TEXP = 0x3000, FBUF = 0x10000;

static uint32_t f2u(float f){ uint32_t u; std::memcpy(&u,&f,4); return u; }

namespace {

struct VtxIn  { float x, y, z; uint32_t rgb; int u, v; };            // model space
struct VtxOut { int32_t x, y, z, q; uint32_t rgb; uint32_t uvp; };   // screen space
struct Frag   { uint32_t w0, w1, w2; };

struct Tb {
  Sim<Vgpu_gfx_top> sim;
  Vgpu_gfx_top* d;
  Tb() : d(sim.dut) {
    d->imem_we=0; d->cbank_we=0; d->gmem_we=0; d->launch=0;
    d->dbg_addr=0; d->dbg_warp=0; d->dbg_cu=0; d->gdbg_addr=0;
    d->grid_nx=0; d->tpb=0; d->entry=0;
    d->r_clear=0; d->r_tri_valid=0; d->r_tex_we=0; d->r_textured=0;
    d->r_tex_bilinear=0; d->r_tex_mip=0; d->r_blend_en=0; d->r_alpha=255; d->r_depth_write=1;
    d->r_emit_mode=0; d->r_fin_valid=0; d->r_fb_raddr=0; d->r_fdbg_idx=0;
    d->r_q0=0x10000; d->r_q1=0x10000; d->r_q2=0x10000;
    sim.reset();
  }
  void load_program(const GpuBin& bin) {
    for (size_t i = 0; i < bin.code.size(); ++i) {
      d->imem_we=1; d->imem_waddr=(uint32_t)i; d->imem_wdata=bin.code[i]; sim.tick();
    }
    d->imem_we=0; d->entry=bin.entry;
  }
  void setc(uint32_t i, uint32_t v){ d->cbank_we=1; d->cbank_waddr=i; d->cbank_wdata=v; sim.tick(); d->cbank_we=0; }
  void st(uint32_t byte, uint32_t v){ d->gmem_we=1; d->gmem_waddr=byte>>2; d->gmem_wdata=v; sim.tick(); d->gmem_we=0; }
  uint32_t ld(uint32_t byte){ d->gdbg_addr=byte>>2; d->eval(); return d->gdbg_data; }
  void launch(uint32_t grid, uint32_t tpb) {
    d->grid_nx=grid; d->tpb=tpb;
    d->launch=1; sim.tick(); d->launch=0;
    int c=0; while (!d->grid_done && c < 8000000) { sim.tick(); ++c; }
    CHECK(d->grid_done);
  }
  void rwait(){ int c=0; while (d->r_busy && c < RW*RH + 300) { sim.tick(); ++c; } }
  void rclear(){ d->r_clear=1; d->r_clear_color=BG; sim.tick(); d->r_clear=0; rwait(); }
  // submit a triangle of VS OUTPUT vertices in fragment-EMIT mode
  void emit_tri(const VtxOut& a, const VtxOut& b, const VtxOut& c) {
    d->r_x0=a.x; d->r_y0=a.y; d->r_z0=a.z; d->r_q0=a.q;
    d->r_x1=b.x; d->r_y1=b.y; d->r_z1=b.z; d->r_q1=b.q;
    d->r_x2=c.x; d->r_y2=c.y; d->r_z2=c.z; d->r_q2=c.q;
    d->r_r0=(a.rgb>>16)&0xFF; d->r_g0=(a.rgb>>8)&0xFF; d->r_b0=a.rgb&0xFF;
    d->r_r1=(b.rgb>>16)&0xFF; d->r_g1=(b.rgb>>8)&0xFF; d->r_b1=b.rgb&0xFF;
    d->r_r2=(c.rgb>>16)&0xFF; d->r_g2=(c.rgb>>8)&0xFF; d->r_b2=c.rgb&0xFF;
    d->r_u0=(int32_t)(a.uvp & 0xFFFF); d->r_v0=(int32_t)(a.uvp >> 16);
    d->r_u1=(int32_t)(b.uvp & 0xFFFF); d->r_v1=(int32_t)(b.uvp >> 16);
    d->r_u2=(int32_t)(c.uvp & 0xFFFF); d->r_v2=(int32_t)(c.uvp >> 16);
    d->r_textured=0; d->r_cull_backface=0; d->r_emit_mode=1;
    d->r_tri_valid=1; sim.tick(); d->r_tri_valid=0; rwait();
    d->r_emit_mode=0;
  }
};

// ---- golden vertex shader: EXACTLY the fp op sequence of vs_persp.s ----
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

// ---- golden raster emit: EXACTLY raster.sv's scan (bbox, coverage, pinterp, 16b packs) --
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

// ---- golden fragment shader: EXACTLY the integer op sequence of fs_texmod.s ----
uint32_t golden_fs(uint32_t w1, uint32_t w2, const std::vector<uint32_t>& tex) {
  uint32_t tx = ((w1 << 16) >> 24), ty = (w1 >> 24);
  if (tx > 31) tx = 31;                              // clamp addressing (as the kernel)
  if (ty > 31) ty = 31;
  uint32_t texel = tex[(ty << 5) + tx];
  uint32_t tr = (texel << 8) >> 24, vr = (w2 << 8) >> 24;
  uint32_t tg = (texel << 16) >> 24, vg = (w2 << 16) >> 24;
  uint32_t tb = texel & 255, vb = w2 & 255;
  uint32_t r = (tr * vr) >> 8, g = (tg * vg) >> 8, bl = (tb * vb) >> 8;
  return (r << 16) | (g << 8) | bl;
}

} // namespace

TEST(shader_pipeline_end_to_end) {
  GpuBin vs, fs; std::string err;
  if (!load_gpubin("build/vs_persp.gpubin", vs, &err)) throw tf::AssertFail("load vs: " + err);
  if (!load_gpubin("build/fs_texmod.gpubin", fs, &err)) throw tf::AssertFail("load fs: " + err);

  // ---- scene: a tilted textured quad (far) + a smaller translucent quad (near) ----
  const float FF = 120.0f, CX = 48.0f, CY = 48.0f, ZOFF = 3.0f;
  std::vector<VtxIn> verts = {
    // quad A (white -> texture as-is), receding in z: near edge y=-0.5 z=0.2, far y=0.9 z=3.0
    {-1.2f, -0.5f, 0.2f, 0xFFFFFF,  0, 32}, { 1.2f, -0.5f, 0.2f, 0xFFFFFF, 32, 32},
    { 1.2f,  0.9f, 3.0f, 0xFFFFFF, 32,  0}, {-1.2f,  0.9f, 3.0f, 0xFFFFFF,  0,  0},
    // quad B (light blue tint), near and small, z=0.0
    {-0.30f, -0.40f, 0.0f, 0x80A0FF, 0, 0}, { 0.42f, -0.40f, 0.0f, 0x80A0FF, 32, 0},
    { 0.42f,  0.30f, 0.0f, 0x80A0FF, 32, 32}, {-0.30f,  0.30f, 0.0f, 0x80A0FF, 0, 32},
  };
  const uint32_t NV = (uint32_t)verts.size();
  int tris[4][3] = {{0,1,2},{0,2,3},{4,5,6},{4,6,7}};   // A then B (B drawn last, blended)

  // texture: 32x32 checker (words 0x00RRGGBB)
  std::vector<uint32_t> tex(TW*TW);
  for (int y = 0; y < TW; ++y) for (int x = 0; x < TW; ++x)
    tex[y*TW + x] = (((x/4)+(y/4)) & 1) ? 0xF0E060 : 0x3050A0;

  Tb tb;

  // =================== 1) VERTEX SHADER on the CU ===================
  tb.load_program(vs);
  for (uint32_t i = 0; i < NV; ++i) {
    uint32_t base = VIN + i*24;
    tb.st(base,    f2u(verts[i].x)); tb.st(base+4,  f2u(verts[i].y));
    tb.st(base+8,  f2u(verts[i].z)); tb.st(base+12, verts[i].rgb);
    tb.st(base+16, (uint32_t)(verts[i].u << 8)); tb.st(base+20, (uint32_t)(verts[i].v << 8));
  }
  tb.setc(0, VIN); tb.setc(1, VOUT); tb.setc(2, NV);
  tb.setc(3, f2u(FF)); tb.setc(4, f2u(CX)); tb.setc(5, f2u(CY)); tb.setc(6, f2u(ZOFF));
  tb.setc(7, f2u(65536.0f)); tb.setc(8, f2u(256.0f));
  tb.launch(1, NV);                                        // one block covers all vertices

  std::vector<VtxOut> vout(NV), gold_v(NV);
  for (uint32_t i = 0; i < NV; ++i) {
    uint32_t base = VOUT + i*24;
    vout[i].x = (int32_t)tb.ld(base);      vout[i].y = (int32_t)tb.ld(base+4);
    vout[i].z = (int32_t)tb.ld(base+8);    vout[i].q = (int32_t)tb.ld(base+12);
    vout[i].rgb = tb.ld(base+16);          vout[i].uvp = tb.ld(base+20);
    gold_v[i] = golden_vs(verts[i], FF, CX, CY, ZOFF);
    CHECK_EQ((uint32_t)vout[i].x, (uint32_t)gold_v[i].x);   // VS on CU == golden fp refs
    CHECK_EQ((uint32_t)vout[i].y, (uint32_t)gold_v[i].y);
    CHECK_EQ((uint32_t)vout[i].z, (uint32_t)gold_v[i].z);
    CHECK_EQ((uint32_t)vout[i].q, (uint32_t)gold_v[i].q);
    CHECK_EQ(vout[i].rgb, gold_v[i].rgb);
    CHECK_EQ(vout[i].uvp, gold_v[i].uvp);
  }

  // =================== 2) RASTER: fragment emission ===================
  tb.rclear();
  std::vector<Frag> gold_frags;
  for (auto& t : tris) {
    tb.emit_tri(vout[t[0]], vout[t[1]], vout[t[2]]);
    golden_emit(gold_frags, vout[t[0]], vout[t[1]], vout[t[2]]);
  }
  uint32_t nfrag = tb.d->r_frag_count;
  CHECK_EQ(nfrag, (uint32_t)gold_frags.size());
  std::vector<Frag> frags(nfrag);
  for (uint32_t i = 0; i < nfrag; ++i) {
    tb.d->r_fdbg_idx = i; tb.d->eval();
    frags[i] = {tb.d->r_fdbg_w0, tb.d->r_fdbg_w1, tb.d->r_fdbg_w2};
    CHECK_EQ(frags[i].w0, gold_frags[i].w0);
    CHECK_EQ(frags[i].w1, gold_frags[i].w1);
    CHECK_EQ(frags[i].w2, gold_frags[i].w2);
  }
  std::printf("  [gfx] %u fragments emitted\n", nfrag);

  // =================== 3) FRAGMENT SHADER on the CU ===================
  for (int i = 0; i < TW*TW; ++i) tb.st(TEXP + 4*i, tex[i]);
  for (uint32_t i = 0; i < nfrag; ++i) {
    tb.st(FBUF + i*12,     frags[i].w0);
    tb.st(FBUF + i*12 + 4, frags[i].w1);
    tb.st(FBUF + i*12 + 8, frags[i].w2);
  }
  tb.load_program(fs);
  tb.setc(0, FBUF); tb.setc(1, nfrag); tb.setc(2, TEXP);
  tb.launch((nfrag + 255) / 256, 256);

  std::vector<uint32_t> shaded(nfrag);
  for (uint32_t i = 0; i < nfrag; ++i) {
    shaded[i] = tb.ld(FBUF + i*12 + 8);
    CHECK_EQ(shaded[i], golden_fs(frags[i].w1, frags[i].w2, tex));  // FS on CU == golden
  }

  // =================== 4) ROP: depth test + blend + write ===================
  // quad A fragments opaque; quad B fragments blended at alpha 160, no depth write.
  // (fragment order preserves triangle order, so the A/B split is by pixel count of A.)
  std::vector<Frag> a_frags;
  for (auto& t : (int[2][3]){{0,1,2},{0,2,3}})
    golden_emit(a_frags, vout[t[0]], vout[t[1]], vout[t[2]]);
  uint32_t na = (uint32_t)a_frags.size();

  gfx::Framebuffer gold_fb; gold_fb.init(RW, RH, BG);
  auto rop_gold = [&](uint32_t i, bool blend, int alpha, bool dw) {
    uint32_t pidx = frags[i].w0 & 0xFFFF;
    int32_t  z    = (int32_t)(frags[i].w0 >> 16);
    int x = pidx % RW, y = pidx / RW;
    size_t di = (size_t)y*RW + x;
    if (z < gold_fb.depth[di]) {
      uint32_t c = shaded[i];
      int r=(c>>16)&0xFF, g=(c>>8)&0xFF, b=c&0xFF;
      if (blend) {
        size_t ci = di*3;
        r = (r*alpha + gold_fb.color[ci+0]*(255-alpha)) / 255;
        g = (g*alpha + gold_fb.color[ci+1]*(255-alpha)) / 255;
        b = (b*alpha + gold_fb.color[ci+2]*(255-alpha)) / 255;
      }
      gold_fb.put(x, y, r, g, b);
      if (dw) gold_fb.depth[di] = z;
    }
  };
  auto rop_rtl = [&](uint32_t i, bool blend, int alpha, bool dw) {
    tb.d->r_blend_en = blend; tb.d->r_alpha = (uint8_t)alpha; tb.d->r_depth_write = dw;
    tb.d->r_fin_pidx = frags[i].w0 & 0xFFFF;
    tb.d->r_fin_z    = (int32_t)(frags[i].w0 >> 16);
    tb.d->r_fin_color= shaded[i] & 0xFFFFFF;
    tb.d->r_fin_valid = 1; tb.sim.tick(); tb.d->r_fin_valid = 0;
  };
  for (uint32_t i = 0; i < na; ++i)    { rop_rtl(i, false, 255, true);  rop_gold(i, false, 255, true); }
  for (uint32_t i = na; i < nfrag; ++i){ rop_rtl(i, true,  160, false); rop_gold(i, true,  160, false); }

  // =================== final: pixel-exact framebuffer ===================
  int mism = 0;
  gfx::Framebuffer rtl_fb; rtl_fb.init(RW, RH, BG);
  for (int i = 0; i < RW*RH; ++i) {
    tb.d->r_fb_raddr = i; tb.d->eval();
    uint32_t c = tb.d->r_fb_rcolor;
    rtl_fb.color[3*i+0]=(c>>16)&0xFF; rtl_fb.color[3*i+1]=(c>>8)&0xFF; rtl_fb.color[3*i+2]=c&0xFF;
    if (rtl_fb.color[3*i] != gold_fb.color[3*i] || rtl_fb.color[3*i+1] != gold_fb.color[3*i+1]
        || rtl_fb.color[3*i+2] != gold_fb.color[3*i+2]) ++mism;
  }
  CHECK_EQ(mism, 0);
  gfx::write_ppm(rtl_fb, "build/render_shaded_rtl.ppm");
  std::printf("  wrote build/render_shaded_rtl.ppm (VS+FS ran as kernels on the Sirion CU)\n");
}

int main(int, char**) { return tf::run_all(); }
