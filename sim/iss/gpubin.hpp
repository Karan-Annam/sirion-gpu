// gpubin.hpp — Sirion object/executable format (.gpubin) reader (C++ side).
//
// Layout (all little-endian):
//   char     magic[4] = "SGPU"
//   uint16   version  = 1
//   uint16   flags    = 0
//   uint32   num_insns
//   uint32   num_const   (static constant words baked into the file)
//   uint32   entry       (entry word index, usually 0)
//   uint32   reg_count   (max vector regs used — metadata)
//   uint32   shared_bytes(per-workgroup shared memory bytes — metadata)
//   uint32   reserved
//   uint32   code[num_insns]
//   uint32   const[num_const]
//
// Kernel parameters (pointers/scalars) are supplied at launch by the host into the
// constant bank starting at word 0 (see iss.hpp Grid::const_bank); static .const data
// from the file follows. v1 is position-independent (word-relative branches), so there
// are no relocations yet — `flags`/`version` reserve room for them later.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>

namespace sirion {

struct GpuBin {
  uint16_t version = 0;
  uint16_t flags = 0;
  uint32_t entry = 0;
  uint32_t reg_count = 0;
  uint32_t shared_bytes = 0;
  std::vector<uint32_t> code;    // instruction words
  std::vector<uint32_t> kconst;  // static constant words from the file
};

inline uint32_t rd_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Load a .gpubin. Returns true on success; on failure sets *err.
inline bool load_gpubin(const std::string& path, GpuBin& out, std::string* err = nullptr) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { if (err) { *err = "cannot open " + path; } return false; }
  std::vector<uint8_t> buf;
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  buf.resize(sz > 0 ? (size_t)sz : 0);
  if (sz > 0 && std::fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) {
    std::fclose(f); if (err) { *err = "short read"; } return false;
  }
  std::fclose(f);
  if (buf.size() < 32) { if (err) { *err = "file too small"; } return false; }
  if (!(buf[0]=='S' && buf[1]=='G' && buf[2]=='P' && buf[3]=='U')) {
    if (err) { *err = "bad magic"; }
    return false;
  }
  out.version      = (uint16_t)(buf[4] | (buf[5] << 8));
  out.flags        = (uint16_t)(buf[6] | (buf[7] << 8));
  uint32_t n_insn  = rd_u32(&buf[8]);
  uint32_t n_const = rd_u32(&buf[12]);
  out.entry        = rd_u32(&buf[16]);
  out.reg_count    = rd_u32(&buf[20]);
  out.shared_bytes = rd_u32(&buf[24]);
  size_t need = 32 + (size_t)n_insn * 4 + (size_t)n_const * 4;
  if (buf.size() < need) { if (err) { *err = "truncated body"; } return false; }
  out.code.resize(n_insn);
  out.kconst.resize(n_const);
  size_t off = 32;
  for (uint32_t i = 0; i < n_insn; ++i, off += 4)  out.code[i]   = rd_u32(&buf[off]);
  for (uint32_t i = 0; i < n_const; ++i, off += 4) out.kconst[i] = rd_u32(&buf[off]);
  return true;
}

} // namespace sirion
