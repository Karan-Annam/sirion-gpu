// test_alu.cpp — self-checking testbench for rtl/compute/alu.sv (M2).
//
// The ALU is purely combinational, so we drive inputs and eval() directly (no clock).
// Random ops are checked against alu_ref() — the SAME golden function the ISS uses — so
// RTL and the golden model provably agree.
#include "Valu.h"
#include "../rtl_driver.hpp"     // provides the sc_time_stamp stub (built with --trace)
#include "../iss/isa.hpp"
#include "test_framework.hpp"
#include <random>

using namespace sirion;

static uint32_t rtl_alu(Valu* d, uint8_t op, uint32_t a, uint32_t b, bool* is_alu = nullptr) {
  d->opcode = op; d->a = a; d->b = b;
  d->eval();
  if (is_alu) *is_alu = d->is_alu;
  return d->y;
}

TEST(alu_directed) {
  VerilatedContext ctx; Valu* d = new Valu(&ctx);
  struct V { uint8_t op; uint32_t a, b, exp; };
  const V vs[] = {
    {OP_ADD, 5, 7, 12}, {OP_ADD, 0xFFFFFFFF, 1, 0}, {OP_SUB, 10, 3, 7},
    {OP_SUB, 3, 10, (uint32_t)-7}, {OP_MUL, 6, 7, 42},
    {OP_MUL, 0x10000, 0x10000, 0}, {OP_MULH, 0x10000, 0x10000, 1},
    {OP_MULH, (uint32_t)-2, 3, 0xFFFFFFFF}, {OP_AND, 0xF0, 0x3C, 0x30},
    {OP_OR, 0xF0, 0x0F, 0xFF}, {OP_XOR, 0xFF, 0x0F, 0xF0}, {OP_NOT, 0x0, 0, 0xFFFFFFFF},
    {OP_SHL, 1, 4, 16}, {OP_SHL, 1, 36, 16}, /*shamt masked to 4*/
    {OP_SHR, 0x80000000, 4, 0x08000000}, {OP_SRA, 0x80000000, 4, 0xF8000000},
    {OP_SLT, (uint32_t)-1, 0, 1}, {OP_SLTU, (uint32_t)-1, 0, 0},
    {OP_MIN, (uint32_t)-5, 3, (uint32_t)-5}, {OP_MAX, (uint32_t)-5, 3, 3},
    {OP_SEQ, 9, 9, 1}, {OP_SEQ, 9, 8, 0}, {OP_MOV, 0x1234, 0xABCD, 0x1234},
  };
  for (const auto& t : vs) CHECK_EQ(rtl_alu(d, t.op, t.a, t.b), t.exp);
  delete d;
}

TEST(alu_random_vs_ref) {
  VerilatedContext ctx; Valu* d = new Valu(&ctx);
  const uint8_t ops[] = {
    OP_ADD, OP_SUB, OP_MUL, OP_MULH, OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_SHL, OP_SHR, OP_SRA, OP_SLT, OP_SLTU, OP_MIN, OP_MAX, OP_SEQ, OP_MOV,
    OP_ADDI, OP_ANDI, OP_ORI, OP_XORI, OP_SHLI, OP_SHRI, OP_SRAI, OP_SLTI, OP_SLTIU, OP_MOVI,
  };
  std::mt19937 rng(0xA1U);
  for (int i = 0; i < 20000; ++i) {
    uint8_t op = ops[rng() % (sizeof(ops) / sizeof(ops[0]))];
    uint32_t a = rng(), b = rng();
    bool is_alu = false;
    uint32_t got = rtl_alu(d, op, a, b, &is_alu);
    CHECK(is_alu);
    CHECK_EQ(got, alu_ref(op, a, b));
  }
  delete d;
}

// Non-ALU opcodes must deassert is_alu (so the execute stage routes them elsewhere).
TEST(alu_is_alu_flag) {
  VerilatedContext ctx; Valu* d = new Valu(&ctx);
  bool f;
  rtl_alu(d, OP_BRA, 1, 2, &f); CHECK(!f);
  rtl_alu(d, OP_LDG, 1, 2, &f); CHECK(!f);
  rtl_alu(d, OP_ISETP, 1, 2, &f); CHECK(!f);
  rtl_alu(d, OP_ADD, 1, 2, &f); CHECK(f);
  delete d;
}

int main(int, char**) { return tf::run_all(); }
