// test_decode.cpp — self-checking testbench for rtl/compute/decode.sv (M2).
//
// Drives random instruction words (mixing valid opcodes and reserved encodings) and
// checks every decoded field/flag against decode_ref() — the golden control decode
// shared with the ISS. Combinational: drive + eval, no clock.
#include "Vdecode.h"
#include "../rtl_driver.hpp"      // sc_time_stamp stub
#include "../iss/isa.hpp"
#include "test_framework.hpp"
#include <random>

using namespace sirion;

static void check_one(Vdecode* d, uint32_t raw) {
  d->insn = raw;
  d->eval();
  DecodeInfo r = decode_ref(raw);
  CHECK_EQ((int)d->opcode, (int)r.opcode);
  CHECK_EQ((int)d->gneg, (int)r.gneg);
  CHECK_EQ((int)d->psel, (int)r.psel);
  CHECK_EQ((int)d->rd, (int)r.rd);
  CHECK_EQ((int)d->rs1, (int)r.rs1);
  CHECK_EQ((int)d->rs2, (int)r.rs2);
  CHECK_EQ((int)d->pd, (int)r.pd);
  CHECK_EQ((int)d->cmp, (int)r.cmp);
  CHECK_EQ(d->imm, (uint32_t)r.imm);
  CHECK_EQ((int)d->reg_write, (int)r.reg_write);
  CHECK_EQ((int)d->is_alu, (int)r.is_alu);
  CHECK_EQ((int)d->is_load, (int)r.is_load);
  CHECK_EQ((int)d->is_store, (int)r.is_store);
  CHECK_EQ((int)d->mem_shared, (int)r.mem_shared);
  CHECK_EQ((int)d->is_branch, (int)r.is_branch);
  CHECK_EQ((int)d->is_ssy, (int)r.is_ssy);
  CHECK_EQ((int)d->is_setp, (int)r.is_setp);
  CHECK_EQ((int)d->is_rdsr, (int)r.is_rdsr);
  CHECK_EQ((int)d->uses_rs1, (int)r.uses_rs1);
  CHECK_EQ((int)d->uses_rs2, (int)r.uses_rs2);
  CHECK_EQ((int)d->alu_a_imm, (int)r.alu_a_imm);
  CHECK_EQ((int)d->alu_b_imm, (int)r.alu_b_imm);
}

// Every defined opcode, with random low bits, plus fully random words.
TEST(decode_all_opcodes) {
  VerilatedContext ctx; Vdecode* d = new Vdecode(&ctx);
  const uint8_t ops[] = {
    OP_NOP, OP_EXIT, OP_BAR, OP_BRA, OP_SSY, OP_SYNC, OP_CALL, OP_RET, OP_RDSR,
    OP_ADD, OP_SUB, OP_MUL, OP_MULH, OP_AND, OP_OR, OP_XOR, OP_NOT, OP_SHL, OP_SHR,
    OP_SRA, OP_SLT, OP_SLTU, OP_MIN, OP_MAX, OP_SEQ, OP_ADDI, OP_ANDI, OP_ORI, OP_XORI,
    OP_SHLI, OP_SHRI, OP_SRAI, OP_SLTI, OP_SLTIU, OP_MOVI, OP_MOV, OP_ISETP, OP_ISETPI,
    OP_LDG, OP_STG, OP_LDS, OP_STS, OP_LDC,
  };
  std::mt19937 rng(0xDEC0DEu);
  for (uint8_t op : ops)
    for (int k = 0; k < 200; ++k)
      check_one(d, ((uint32_t)op << 26) | (rng() & 0x03FFFFFF));
  delete d;
}

TEST(decode_random_words) {
  VerilatedContext ctx; Vdecode* d = new Vdecode(&ctx);
  std::mt19937 rng(0x5A5A5A5Au);
  for (int i = 0; i < 20000; ++i) check_one(d, rng());
  delete d;
}

int main(int, char**) { return tf::run_all(); }
