// render_demo.cpp — render a scene with the Sirion graphics golden model (M9).
//
// Host-side vertex stage (rotate + perspective-project a colored cube) feeds the integer
// rasterizer in sim/gfx/gfx.hpp, which does coverage + barycentric Gouraud interpolation +
// depth test into a framebuffer, dumped as a PPM. `scripts/ppm_tool.py` previews/exports it.
#include "../gfx/gfx.hpp"
#include <cmath>
#include <cstdio>
#include <array>

using namespace sirion::gfx;

struct V3 { float x, y, z; };

static V3 rot(V3 p, float ax, float ay) {
  // rotate around Y then X
  float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay);
  float x1 =  cy * p.x + sy * p.z;
  float z1 = -sy * p.x + cy * p.z;
  float y2 =  cx * p.y - sx * z1;
  float z2 =  sx * p.y + cx * z1;
  return {x1, y2, z2};
}

int main(int argc, char** argv) {
  const int W = 220, H = 220;
  const char* out = (argc > 1) ? argv[1] : "build/render.ppm";
  float ax = (argc > 2) ? std::atof(argv[2]) : 0.6f;
  float ay = (argc > 3) ? std::atof(argv[3]) : 0.8f;

  Framebuffer fb; fb.init(W, H, 0x101020);   // dark blue background

  // Cube: 8 corners at (+-1) with a rainbow color per corner.
  std::array<V3, 8> P = {{ {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                           {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} }};
  auto col = [](V3 p){ return Vtx{0,0,0,
      (int)((p.x*0.5f+0.5f)*255), (int)((p.y*0.5f+0.5f)*255), (int)((p.z*0.5f+0.5f)*255)}; };
  // 12 triangles (6 faces), wound CCW when viewed from outside.
  const int F[12][3] = {
    {0,2,1},{0,3,2},  {4,5,6},{4,6,7},   {0,1,5},{0,5,4},
    {2,3,7},{2,7,6},  {1,2,6},{1,6,5},   {0,4,7},{0,7,3} };

  auto project = [&](V3 p) -> Vtx {
    V3 r = rot(p, ax, ay);
    float zc = r.z + 4.0f;                 // camera distance 4
    float s = 90.0f / zc;                  // perspective scale
    Vtx v = col(p);
    v.x = (int)(W * 0.5f + r.x * s);
    v.y = (int)(H * 0.5f - r.y * s);
    v.z = (int)((zc - 2.5f) / 3.0f * 65535.0f);   // ~near..far -> 0..65535
    return v;
  };

  int px_written = 0;
  for (auto& f : F) {
    Vtx a = project(P[f[0]]), b = project(P[f[1]]), c = project(P[f[2]]);
    px_written += raster_triangle(fb, a, b, c, /*cull_backface=*/true);
  }

  if (!write_ppm(fb, out)) { std::fprintf(stderr, "cannot write %s\n", out); return 1; }
  std::printf("rendered %s (%dx%d), %d fragments written\n", out, W, H, px_written);
  return 0;
}
