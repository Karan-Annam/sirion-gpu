// gpu.hpp — Sirion host graphics API (M10).
//
// A small C++ facade over the graphics pipeline, shaped like the usual host API
// (initialize / create resources / draw / present). It drives the golden rasterizer
// (sim/gfx/gfx.hpp) — the software model of the same fixed-function hardware in
// rtl/graphics/raster.sv — so host programs render without wiring Verilator directly.
#pragma once
#include "../gfx/gfx.hpp"
#include "../gfx/scene.hpp"
#include <string>
#include <vector>

namespace sirion {

class GpuGfx {
public:
  void initialize(int w, int h, uint32_t bg = 0x000000) { bg_ = bg; fb_.init(w, h, bg); }
  void clear() { fb_.init(fb_.W, fb_.H, bg_); }

  // upload a texture (used when a draw is textured)
  void createTexture(const gfx::Texture& t) { tex_ = t; has_tex_ = true; }

  void drawTriangle(const gfx::Vtx& a, const gfx::Vtx& b, const gfx::Vtx& c,
                    bool textured = false, bool cull = true) {
    frags_ += gfx::raster_triangle(fb_, a, b, c, cull, textured && has_tex_ ? &tex_ : nullptr);
    ++tris_;
  }
  void drawMesh(const std::vector<gfx::Tri>& tris, bool textured = false) {
    for (auto& t : tris) drawTriangle(t.v[0], t.v[1], t.v[2], textured, t.cull);
  }

  bool present(const std::string& ppm) { return gfx::write_ppm(fb_, ppm); }

  const gfx::Framebuffer& framebuffer() const { return fb_; }
  long trianglesDrawn() const { return tris_; }
  long fragmentsWritten() const { return frags_; }

private:
  gfx::Framebuffer fb_;
  gfx::Texture tex_;
  bool has_tex_ = false;
  uint32_t bg_ = 0;
  long tris_ = 0, frags_ = 0;
};

}  // namespace sirion
