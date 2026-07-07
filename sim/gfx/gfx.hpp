// gfx.hpp — Sirion graphics golden model (M9): integer triangle rasterizer + framebuffer.
//
// This is the reference the RTL rasterizer (rtl/graphics/raster.sv) is verified against, so
// the arithmetic is deliberately **all integer** (edge functions, barycentric weights, and
// interpolation via integer division) — identical ops in C++ and SystemVerilog, so the two
// match pixel-for-pixel.
//
// Pipeline position: vertices arrive already in screen space (the vertex stage — transform +
// projection — runs on the host/CU and snaps to integer pixel coords, integer depth, integer
// 8-bit colors). The rasterizer does triangle setup, coverage (edge equations), attribute +
// depth interpolation (barycentric), the depth test, and the framebuffer write.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>

namespace sirion {
namespace gfx {

struct Vtx {                 // screen-space vertex (integer)
  int32_t x = 0, y = 0;      // pixel coordinates
  int32_t z = 0;            // depth (smaller = nearer), fixed-point 0..0xFFFF
  int32_t r = 0, g = 0, b = 0; // color, 0..255
  int32_t u = 0, v = 0;     // texture coords, texel-space * 256 (8.8 fixed-point)
  int32_t q = 0x10000;      // 1/w in Q16 (M18 perspective correction; 0x10000 = affine)
};

struct Texture {             // RGB texture with a box-filtered mip pyramid (M18)
  int W = 0, H = 0;
  int levels = 1;            // 1 = no mipmaps
  std::vector<uint8_t> rgb;  // all levels, L0 first; level l is (W>>l) x (H>>l)
  static size_t lvl_off(int W, int l) {           // word offset of level l (square, pow-2)
    size_t o = 0;
    for (int i = 0; i < l; ++i) o += (size_t)(W >> i) * (W >> i);
    return o;
  }
  void checker(int w, int h, int tile, uint32_t c0, uint32_t c1) {
    W = w; H = h; levels = 1; rgb.resize((size_t)w*h*3);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
      uint32_t c = (((x/tile) + (y/tile)) & 1) ? c1 : c0;
      size_t i = ((size_t)y*w + x)*3;
      rgb[i]=(c>>16)&0xFF; rgb[i+1]=(c>>8)&0xFF; rgb[i+2]=c&0xFF;
    }
  }
  // Build the mip pyramid with a 2x2 box filter (call after filling level 0; square pow-2).
  void build_mips() {
    levels = 1;
    while ((W >> levels) >= 1 && (H >> levels) >= 1) ++levels;
    size_t total = lvl_off(W, levels);
    rgb.resize(total * 3);
    for (int l = 1; l < levels; ++l) {
      int sw = W >> (l-1), dw = W >> l;
      size_t so = lvl_off(W, l-1), dst = lvl_off(W, l);
      for (int y = 0; y < dw; ++y) for (int x = 0; x < dw; ++x)
        for (int c = 0; c < 3; ++c) {
          int s = rgb[(so + (size_t)(2*y)*sw   + 2*x  )*3 + c]
                + rgb[(so + (size_t)(2*y)*sw   + 2*x+1)*3 + c]
                + rgb[(so + (size_t)(2*y+1)*sw + 2*x  )*3 + c]
                + rgb[(so + (size_t)(2*y+1)*sw + 2*x+1)*3 + c];
          rgb[(dst + (size_t)y*dw + x)*3 + c] = (uint8_t)(s >> 2);
        }
    }
  }
  inline void sample_lvl(int l, int tx, int ty, int& r, int& g, int& b) const {
    int lw = W >> l, lh = H >> l;
    tx = tx < 0 ? 0 : tx >= lw ? lw-1 : tx;   // clamp addressing
    ty = ty < 0 ? 0 : ty >= lh ? lh-1 : ty;
    size_t i = (lvl_off(W, l) + (size_t)ty*lw + tx) * 3;
    r = rgb[i]; g = rgb[i+1]; b = rgb[i+2];
  }
  inline void sample(int tx, int ty, int& r, int& g, int& b) const {  // nearest, level 0
    sample_lvl(0, tx, ty, r, g, b);
  }
  // Bilinear sample at 8.8 texel coordinates in level l (matches the RTL bit-for-bit):
  // half-texel center offset, Q8 weights, >>16 blend.
  inline void sample_bilinear(int l, int32_t u88, int32_t v88, int& r, int& g, int& b) const {
    int32_t uu = (u88 >> l) - 128, vv = (v88 >> l) - 128;   // level scale + center offset
    int32_t tx = uu >> 8, ty = vv >> 8;
    int32_t fx = uu & 0xFF, fy = vv & 0xFF;
    int r00,g00,b00, r10,g10,b10, r01,g01,b01, r11,g11,b11;
    sample_lvl(l, tx,   ty,   r00,g00,b00);
    sample_lvl(l, tx+1, ty,   r10,g10,b10);
    sample_lvl(l, tx,   ty+1, r01,g01,b01);
    sample_lvl(l, tx+1, ty+1, r11,g11,b11);
    auto blend = [&](int c00, int c10, int c01, int c11) {
      return (c00*(256-fx)*(256-fy) + c10*fx*(256-fy)
            + c01*(256-fx)*fy       + c11*fx*fy) >> 16;
    };
    r = blend(r00,r10,r01,r11); g = blend(g00,g10,g01,g11); b = blend(b00,b10,b01,b11);
  }
};

// Per-draw raster/ROP state (M18). Defaults reproduce the M9/M10 behavior exactly.
struct RasterState {
  bool cull_backface = false;
  const Texture* tex = nullptr;
  bool bilinear   = false;   // bilinear filtering (else nearest, level 0)
  bool mipmapped  = false;   // per-triangle LOD selection into the pyramid
  bool blend      = false;   // alpha blend: C = (Cs*a + Cd*(255-a)) / 255
  int  alpha      = 255;
  bool depth_write = true;   // translucent passes typically test but don't write depth
};

struct Framebuffer {
  int W = 0, H = 0;
  std::vector<uint8_t>  color;   // RGB8, W*H*3
  std::vector<int32_t>  depth;   // W*H, initialized to far

  void init(int w, int h, int32_t clear_rgb = 0x000000, int32_t far_z = 0x7FFFFFFF) {
    W = w; H = h;
    color.assign((size_t)W * H * 3, 0);
    depth.assign((size_t)W * H, far_z);
    for (int i = 0; i < W * H; ++i) {
      color[3*i+0] = (clear_rgb >> 16) & 0xFF;
      color[3*i+1] = (clear_rgb >> 8) & 0xFF;
      color[3*i+2] = clear_rgb & 0xFF;
    }
  }
  inline void put(int x, int y, int r, int g, int b) {
    size_t i = ((size_t)y * W + x) * 3;
    color[i+0] = (uint8_t)r; color[i+1] = (uint8_t)g; color[i+2] = (uint8_t)b;
  }
};

// Edge function: cross product of (b-a) and (p-a). Sign gives which side of edge a->b p is on.
inline int64_t edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) {
  return (int64_t)(bx - ax) * (py - ay) - (int64_t)(by - ay) * (px - ax);
}

// Rasterize one triangle into fb with the M18 feature set. All-integer math, mirrored
// bit-for-bit by rtl/graphics/raster.sv:
//  * PERSPECTIVE-CORRECT attributes: attr = sum(wi*ai*qi) / sum(wi*qi) with qi = 1/w (Q16).
//    With equal qi this reduces EXACTLY to the old affine sum(wi*ai)/area (the identity
//    w0+w1+w2 = area), so M9/M10 scenes are unchanged. Depth stays screen-linear (Z-buffer).
//  * texturing: nearest or BILINEAR, optional per-triangle MIP level from the ratio of
//    texel area to pixel area (each level covers 4x the area).
//  * ROP: depth test, optional depth write, optional alpha BLEND (Cs*a + Cd*(255-a))/255.
// Returns fragments written.
inline int raster_triangle_ex(Framebuffer& fb, const Vtx& v0, const Vtx& v1, const Vtx& v2,
                              const RasterState& st) {
  int minx = std::max(0,       std::min({v0.x, v1.x, v2.x}));
  int maxx = std::min(fb.W - 1, std::max({v0.x, v1.x, v2.x}));
  int miny = std::max(0,       std::min({v0.y, v1.y, v2.y}));
  int maxy = std::min(fb.H - 1, std::max({v0.y, v1.y, v2.y}));

  int64_t area = edge(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);  // 2*signed area
  if (area == 0) return 0;
  if (st.cull_backface && area < 0) return 0;
  bool pos = area > 0;

  // per-triangle mip level: texel-area / pixel-area ratio; level l covers 4^l texels/pixel
  int lod = 0;
  if (st.tex && st.mipmapped) {
    int64_t du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
    int64_t du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
    uint64_t ta = (uint64_t)std::llabs(du1 * dv2 - du2 * dv1);   // Q16 texel^2
    uint64_t pa = (uint64_t)std::llabs(area) << 16;
    uint64_t ratio = pa ? ta / pa : 0;
    while (lod + 1 < st.tex->levels && (ratio >> (2 * (lod + 1))) != 0) ++lod;
  }

  int written = 0;
  for (int py = miny; py <= maxy; ++py) {
    for (int px = minx; px <= maxx; ++px) {
      int64_t w0 = edge(v1.x, v1.y, v2.x, v2.y, px, py);   // opposite v0
      int64_t w1 = edge(v2.x, v2.y, v0.x, v0.y, px, py);   // opposite v1
      int64_t w2 = edge(v0.x, v0.y, v1.x, v1.y, px, py);   // opposite v2
      bool inside = pos ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                        : (w0 <= 0 && w1 <= 0 && w2 <= 0);
      if (!inside) continue;
      int32_t z = (int32_t)((w0 * v0.z + w1 * v1.z + w2 * v2.z) / area);
      size_t di = (size_t)py * fb.W + px;
      if (z < fb.depth[di]) {
        // perspective-correct barycentric weights (Q16 1/w per vertex)
        int64_t pw = w0 * v0.q + w1 * v1.q + w2 * v2.q;
        if (pw == 0) continue;
        auto pinterp = [&](int64_t a0, int64_t a1, int64_t a2) -> int32_t {
          return (int32_t)((w0 * a0 * v0.q + w1 * a1 * v1.q + w2 * a2 * v2.q) / pw);
        };
        int32_t r, g, b;
        if (st.tex) {
          int32_t ui = pinterp(v0.u, v1.u, v2.u);
          int32_t vi = pinterp(v0.v, v1.v, v2.v);
          int R, G, B;
          if (st.bilinear) st.tex->sample_bilinear(lod, ui, vi, R, G, B);
          else             st.tex->sample_lvl(lod, (ui >> (8 + lod)), (vi >> (8 + lod)), R, G, B);
          r = R; g = G; b = B;
        } else {
          r = pinterp(v0.r, v1.r, v2.r);
          g = pinterp(v0.g, v1.g, v2.g);
          b = pinterp(v0.b, v1.b, v2.b);
        }
        if (st.blend) {                                   // ROP alpha blend
          size_t ci = di * 3;
          r = (r * st.alpha + fb.color[ci+0] * (255 - st.alpha)) / 255;
          g = (g * st.alpha + fb.color[ci+1] * (255 - st.alpha)) / 255;
          b = (b * st.alpha + fb.color[ci+2] * (255 - st.alpha)) / 255;
        }
        fb.put(px, py, r, g, b);
        if (st.depth_write) fb.depth[di] = z;
        ++written;
      }
    }
  }
  return written;
}

// M9/M10-compatible wrapper (affine, nearest, opaque).
inline int raster_triangle(Framebuffer& fb, const Vtx& v0, const Vtx& v1, const Vtx& v2,
                           bool cull_backface = false, const Texture* tex = nullptr) {
  RasterState st;
  st.cull_backface = cull_backface;
  st.tex = tex;
  return raster_triangle_ex(fb, v0, v1, v2, st);
}

// Write a binary PPM (P6). Viewable directly, and by scripts/ppm_tool.py (PNG + ASCII preview).
inline bool write_ppm(const Framebuffer& fb, const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", fb.W, fb.H);
  std::fwrite(fb.color.data(), 1, fb.color.size(), f);
  std::fclose(f);
  return true;
}

}  // namespace gfx
}  // namespace sirion
