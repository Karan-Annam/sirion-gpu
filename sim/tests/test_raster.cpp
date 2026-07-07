// test_raster.cpp — verify rtl/graphics/raster.sv against the golden rasterizer and render
// on the RTL hardware. M9/M10 scenes (Gouraud + textured cube) plus the M18 feature set:
// perspective-correct interpolation, bilinear + mipmapped texturing, and ROP alpha blending.
//
// Every case is pixel-exact RTL vs golden; the M18 cases also carry known-answer probes
// (e.g. the perspective-correct result must DIFFER from the affine one where it should,
// and a blended pixel must equal the closed-form blend).
#include "Vraster.h"
#include "../rtl_driver.hpp"
#include "../gfx/gfx.hpp"
#include "../gfx/scene.hpp"
#include "test_framework.hpp"

using namespace sirion;
using namespace sirion::gfx;

static constexpr int W = 220, H = 220;
static constexpr uint32_t BG = 0x101020;

namespace {

struct RasterTb {
  Sim<Vraster> sim;
  Vraster* d;
  RasterTb() : d(sim.dut) {
    d->clear = 0; d->tri_valid = 0; d->fb_raddr = 0; d->tex_we = 0; d->textured = 0;
    d->tex_bilinear = 0; d->tex_mip = 0; d->blend_en = 0; d->alpha = 255; d->depth_write = 1;
    d->q0 = 0x10000; d->q1 = 0x10000; d->q2 = 0x10000;
    sim.reset();
  }
  void wait_idle() { int c = 0; while (d->busy && c < W*H + 300) { sim.tick(); ++c; } }
  void clear() { d->clear = 1; d->clear_color = BG; sim.tick(); d->clear = 0; wait_idle(); }
  void load_texture(const Texture& tex) {          // uploads the WHOLE pyramid
    size_t words = tex.rgb.size() / 3;
    for (size_t i = 0; i < words; ++i) {
      uint32_t c = (tex.rgb[3*i]<<16) | (tex.rgb[3*i+1]<<8) | tex.rgb[3*i+2];
      d->tex_we = 1; d->tex_waddr = (uint32_t)i; d->tex_wdata = c; sim.tick(); d->tex_we = 0;
    }
  }
  void submit(const Vtx& a, const Vtx& b, const Vtx& c, const RasterState& st) {
    CHECK_EQ((int)d->busy, 0);
    d->x0=a.x; d->y0=a.y; d->z0=a.z; d->r0=a.r; d->g0=a.g; d->b0=a.b; d->u0=a.u; d->v0=a.v; d->q0=a.q;
    d->x1=b.x; d->y1=b.y; d->z1=b.z; d->r1=b.r; d->g1=b.g; d->b1=b.b; d->u1=b.u; d->v1=b.v; d->q1=b.q;
    d->x2=c.x; d->y2=c.y; d->z2=c.z; d->r2=c.r; d->g2=c.g; d->b2=c.b; d->u2=c.u; d->v2=c.v; d->q2=c.q;
    d->textured = st.tex ? 1 : 0;
    d->tex_bilinear = st.bilinear ? 1 : 0;
    d->tex_mip = st.mipmapped ? 1 : 0;
    d->blend_en = st.blend ? 1 : 0;
    d->alpha = (uint8_t)st.alpha;
    d->depth_write = st.depth_write ? 1 : 0;
    d->cull_backface = st.cull_backface ? 1 : 0;
    d->tri_valid = 1; sim.tick(); d->tri_valid = 0; wait_idle();
  }
  void readback(Framebuffer& fb) {
    fb.init(W, H, BG);
    for (int i = 0; i < W*H; ++i) {
      d->fb_raddr = i; d->eval(); uint32_t c = d->fb_rcolor;
      fb.color[3*i+0]=(c>>16)&0xFF; fb.color[3*i+1]=(c>>8)&0xFF; fb.color[3*i+2]=c&0xFF;
    }
  }
};

int diff(const Framebuffer& a, const Framebuffer& b) {
  int m = 0;
  for (size_t i = 0; i < a.color.size(); ++i) if (a.color[i] != b.color[i]) ++m;
  return m;
}

} // namespace

// ---- M9/M10 scenes (affine, nearest, opaque) — unchanged behavior ----
static void render_scene(bool textured, const Texture* tex, const char* ppm) {
  auto tris = cube_scene(W, H, 0.6f, 0.8f);
  RasterTb tb;
  if (textured && tex) tb.load_texture(*tex);

  for (size_t k = 0; k < tris.size(); ++k) {
    RasterState st; st.cull_backface = tris[k].cull; st.tex = textured ? tex : nullptr;
    tb.clear(); tb.submit(tris[k].v[0], tris[k].v[1], tris[k].v[2], st);
    Framebuffer g; g.init(W,H,BG);
    raster_triangle_ex(g, tris[k].v[0], tris[k].v[1], tris[k].v[2], st);
    Framebuffer r; tb.readback(r);
    CHECK_EQ(diff(g, r), 0);
  }

  Framebuffer gold; gold.init(W, H, BG);
  tb.clear();
  for (auto& t : tris) {
    RasterState st; st.cull_backface = t.cull; st.tex = textured ? tex : nullptr;
    tb.submit(t.v[0], t.v[1], t.v[2], st);
    raster_triangle_ex(gold, t.v[0], t.v[1], t.v[2], st);
  }
  Framebuffer rtlfb; tb.readback(rtlfb);
  CHECK_EQ(diff(gold, rtlfb), 0);

  write_ppm(rtlfb, ppm);
  std::printf("  wrote %s (rendered by the RTL rasterizer)\n", ppm);
}

TEST(raster_gouraud) { render_scene(false, nullptr, "build/render_rtl.ppm"); }

TEST(raster_textured) {
  Texture tex; tex.checker(64, 64, 8, 0xF0E060, 0x304090);
  render_scene(true, &tex, "build/render_tex_rtl.ppm");
}

// ---- M18: perspective-correct interpolation ----
// A "floor" quad receding into the distance: top edge has q = 1/w four times smaller.
// Pixel-exact vs golden AND provably different from the affine result at the midline.
TEST(raster_m18_perspective) {
  Texture tex; tex.checker(64, 64, 8, 0xFFFFFF, 0x202020); tex.build_mips();
  RasterTb tb; tb.load_texture(tex);

  auto mkv = [](int x, int y, int z, int u, int v, int q) {
    Vtx t; t.x=x; t.y=y; t.z=z; t.u=u<<8; t.v=v<<8; t.q=q; t.r=200; t.g=200; t.b=200; return t;
  };
  // near edge (bottom, w=1 -> q=0x10000), far edge (top, w=4 -> q=0x4000)
  Vtx bl = mkv(30, 200, 100, 0,  64, 0x10000), br = mkv(190, 200, 100, 64, 64, 0x10000);
  Vtx tl = mkv(80, 60, 200, 0, 0, 0x4000),     tr = mkv(140, 60, 200, 64, 0, 0x4000);

  RasterState st; st.tex = &tex;
  Framebuffer gold; gold.init(W, H, BG);
  tb.clear();
  tb.submit(bl, br, tr, st); raster_triangle_ex(gold, bl, br, tr, st);
  tb.submit(bl, tr, tl, st); raster_triangle_ex(gold, bl, tr, tl, st);
  Framebuffer r; tb.readback(r);
  CHECK_EQ(diff(gold, r), 0);                                  // RTL == golden, pixel exact

  // known-answer probe: with perspective, the v=32-texel line sits NEARER the far edge
  // than the affine midline. Compare against an affine render of the same quad.
  RasterState staff = st;
  Vtx abl = bl, abr = br, atl = tl, atr = tr;
  abl.q = abr.q = atl.q = atr.q = 0x10000;
  Framebuffer aff; aff.init(W, H, BG);
  raster_triangle_ex(aff, abl, abr, atr, staff);
  raster_triangle_ex(aff, abl, atr, atl, staff);
  CHECK(diff(gold, aff) > 1000);                               // the correction is live
}

// ---- M18: bilinear filtering ----
TEST(raster_m18_bilinear) {
  Texture tex; tex.checker(64, 64, 4, 0xFF4020, 0x2040FF);
  RasterTb tb; tb.load_texture(tex);
  auto mkv = [](int x, int y, int u, int v) {
    Vtx t; t.x=x; t.y=y; t.z=100; t.u=u; t.v=v; return t;
  };
  // magnified: 16 texels stretched over ~180 pixels -> smooth gradients under bilinear
  Vtx a = mkv(20, 20,   0,      0), b = mkv(200, 20, 16<<8,   0), c = mkv(200, 200, 16<<8, 16<<8);
  RasterState st; st.tex = &tex; st.bilinear = true;
  Framebuffer gold; gold.init(W, H, BG);
  tb.clear();
  tb.submit(a, b, c, st); raster_triangle_ex(gold, a, b, c, st);
  Framebuffer r; tb.readback(r);
  CHECK_EQ(diff(gold, r), 0);
  // known answer: bilinear output contains intermediate values a nearest sample can't produce
  RasterState stn = st; stn.bilinear = false;
  Framebuffer near_fb; near_fb.init(W, H, BG);
  raster_triangle_ex(near_fb, a, b, c, stn);
  CHECK(diff(gold, near_fb) > 1000);
}

// ---- M18: mipmap selection ----
TEST(raster_m18_mipmap) {
  Texture tex; tex.checker(64, 64, 1, 0xFFFFFF, 0x000000);     // 1-texel checker: aliases hard
  tex.build_mips();                                            // level>=1 boxes to mid-gray
  RasterTb tb; tb.load_texture(tex);
  auto mkv = [](int x, int y, int u, int v) {
    Vtx t; t.x=x; t.y=y; t.z=100; t.u=u<<8; t.v=v<<8; return t;
  };
  // minified: the full 64-texel texture squeezed into ~16 pixels -> ratio 16 -> lod 2
  Vtx a = mkv(100, 100, 0, 0), b = mkv(116, 100, 64, 0), c = mkv(116, 116, 64, 64);
  RasterState st; st.tex = &tex; st.mipmapped = true;
  Framebuffer gold; gold.init(W, H, BG);
  tb.clear();
  tb.submit(a, b, c, st); raster_triangle_ex(gold, a, b, c, st);
  Framebuffer r; tb.readback(r);
  CHECK_EQ(diff(gold, r), 0);
  // known answer: at lod>=1 a 1-texel checker boxes to exactly mid-gray 127/128
  int probe = (108 * W + 110) * 3;
  CHECK(gold.color[probe] > 100 && gold.color[probe] < 160);
}

// ---- M18: ROP alpha blending (translucent triangle over an opaque one) ----
TEST(raster_m18_blend) {
  RasterTb tb;
  auto mkv = [](int x, int y, int z, int r, int g, int b) {
    Vtx t; t.x=x; t.y=y; t.z=z; t.r=r; t.g=g; t.b=b; return t;
  };
  // opaque red base
  Vtx a = mkv(20, 20, 200, 255, 0, 0), b = mkv(200, 20, 200, 255, 0, 0),
      c = mkv(110, 200, 200, 255, 0, 0);
  // translucent blue on top (nearer), alpha 128, depth-write off
  Vtx d0 = mkv(60, 40, 100, 0, 0, 255), d1 = mkv(180, 60, 100, 0, 0, 255),
      d2 = mkv(120, 180, 100, 0, 0, 255);

  RasterState opq;
  RasterState bld; bld.blend = true; bld.alpha = 128; bld.depth_write = false;

  Framebuffer gold; gold.init(W, H, BG);
  tb.clear();
  tb.submit(a, b, c, opq);    raster_triangle_ex(gold, a, b, c, opq);
  tb.submit(d0, d1, d2, bld); raster_triangle_ex(gold, d0, d1, d2, bld);
  Framebuffer r; tb.readback(r);
  CHECK_EQ(diff(gold, r), 0);
  // known answer: overlap pixel = (blue*128 + red*127)/255 exactly
  int px = 120, py = 100, i = (py * W + px) * 3;
  CHECK_EQ((int)gold.color[i+0], (0 * 128 + 255 * 127) / 255);   // R
  CHECK_EQ((int)gold.color[i+2], (255 * 128 + 0 * 127) / 255);   // B
}

int main(int, char**) { return tf::run_all(); }
