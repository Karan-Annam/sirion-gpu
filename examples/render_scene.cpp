// render_scene.cpp — a host program that renders with the Sirion host graphics API (M10).
//
//   sirion::GpuGfx gpu;
//   gpu.initialize(...); gpu.createTexture(...); gpu.drawMesh(...); gpu.present("out.ppm");
//
// Renders a texture-mapped, depth-tested, back-face-culled 3D cube and reports the
// triangles/fragments drawn. Preview with: python scripts/ppm_tool.py build/render_api.ppm
#include "../sim/hostapi/gpu.hpp"
#include <cstdio>

int main(int argc, char** argv) {
  const int W = 256, H = 256;
  float ax = (argc > 1) ? std::atof(argv[1]) : 0.6f;
  float ay = (argc > 2) ? std::atof(argv[2]) : 0.8f;
  const char* out = (argc > 3) ? argv[3] : "build/render_api.ppm";

  sirion::GpuGfx gpu;
  gpu.initialize(W, H, 0x101828);

  sirion::gfx::Texture tex;
  tex.checker(64, 64, 8, 0xF0E060, 0x304090);
  gpu.createTexture(tex);

  gpu.drawMesh(sirion::gfx::cube_scene(W, H, ax, ay), /*textured=*/true);

  gpu.present(out);
  std::printf("host API: rendered %s  (%ld triangles, %ld fragments)\n",
              out, gpu.trianglesDrawn(), gpu.fragmentsWritten());
  return 0;
}
