// scene.hpp — the host vertex stage: builds a colored 3D cube, rotates + perspective-projects
// it, and snaps to integer screen-space triangles. Shared by the software render demo and the
// RTL rasterizer test so both rasterize identical vertices.
#pragma once
#include "gfx.hpp"
#include <vector>
#include <array>
#include <cmath>

namespace sirion {
namespace gfx {

struct Tri { Vtx v[3]; bool cull; };

inline std::vector<Tri> cube_scene(int W, int H, float ax, float ay) {
  struct V3 { float x, y, z; };
  auto rot = [&](V3 p) -> V3 {
    float cx=std::cos(ax), sx=std::sin(ax), cy=std::cos(ay), sy=std::sin(ay);
    float x1= cy*p.x + sy*p.z, z1= -sy*p.x + cy*p.z;
    float y2= cx*p.y - sx*z1,  z2=  sx*p.y + cx*z1;
    return {x1, y2, z2};
  };
  std::array<V3,8> P = {{ {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                          {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} }};
  auto col = [](V3 p){ return Vtx{0,0,0,
      (int)((p.x*0.5f+0.5f)*255),(int)((p.y*0.5f+0.5f)*255),(int)((p.z*0.5f+0.5f)*255)}; };
  const int F[12][3] = { {0,2,1},{0,3,2}, {4,5,6},{4,6,7}, {0,1,5},{0,5,4},
                         {2,3,7},{2,7,6}, {1,2,6},{1,6,5}, {0,4,7},{0,7,3} };
  auto project = [&](V3 p) -> Vtx {
    V3 r = rot(p); float zc = r.z + 4.0f, s = 90.0f / zc;
    Vtx v = col(p);
    v.x = (int)(W*0.5f + r.x*s);
    v.y = (int)(H*0.5f - r.y*s);
    v.z = (int)((zc - 2.5f) / 3.0f * 65535.0f);
    return v;
  };
  const int TEX = 64;
  // per-face UV: pick the two in-face axes (the constant axis is the face normal).
  auto setuv = [&](Vtx& vv, V3 p, V3 a, V3 b, V3 c) {
    float uu, w;
    if (a.x == b.x && b.x == c.x)      { uu = (p.z+1)*0.5f; w = (p.y+1)*0.5f; }
    else if (a.y == b.y && b.y == c.y) { uu = (p.x+1)*0.5f; w = (p.z+1)*0.5f; }
    else                               { uu = (p.x+1)*0.5f; w = (p.y+1)*0.5f; }
    vv.u = (int)(uu * (TEX-1) * 256);   // 8.8 texel-space
    vv.v = (int)(w  * (TEX-1) * 256);
  };
  std::vector<Tri> tris;
  for (auto& f : F) {
    Vtx a = project(P[f[0]]), b = project(P[f[1]]), c = project(P[f[2]]);
    setuv(a, P[f[0]], P[f[0]], P[f[1]], P[f[2]]);
    setuv(b, P[f[1]], P[f[0]], P[f[1]], P[f[2]]);
    setuv(c, P[f[2]], P[f[0]], P[f[1]], P[f[2]]);
    tris.push_back({{a, b, c}, true});
  }
  return tris;
}

}  // namespace gfx
}  // namespace sirion
