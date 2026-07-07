#!/usr/bin/env python3
"""asm.py — two-pass assembler for the Sirion SIMT ISA v1 -> .gpubin.

Readable assembly, e.g.:

    ; out[i] = a[i] + b[i]   for i < n
        RDSR   R0, TID_FLAT
        LDC    R1, c[3]           ; n
        ISETP.GE P1, R0, R1       ; P1 = (tid >= n)
        SSY    Ldone
    @P1 BRA    Ldone              ; divergent: inactive lanes skip
        SHLI   R2, R0, 2          ; byte offset = tid*4
        ...
    Ldone:
        EXIT

Encoding is kept in lockstep with sim/iss/isa.hpp and rtl/sirion_pkg.sv; the
assemble->run self-checking tests catch any drift.
"""
import argparse
import re
import struct
import sys

# mnemonic -> (opcode, format)
MNEM = {
    'NOP': (0x00, 'N'), 'EXIT': (0x01, 'N'), 'BAR': (0x02, 'N'),
    'SYNC': (0x05, 'N'), 'RET': (0x07, 'N'),
    'BRA': (0x03, 'B'), 'SSY': (0x04, 'B'), 'CALL': (0x06, 'B'),
    'RDSR': (0x08, 'RDSR'),
    'ADD': (0x10, 'R'), 'SUB': (0x11, 'R'), 'MUL': (0x12, 'R'), 'MULH': (0x13, 'R'),
    'AND': (0x14, 'R'), 'OR': (0x15, 'R'), 'XOR': (0x16, 'R'), 'NOT': (0x17, 'R1'),
    'SHL': (0x18, 'R'), 'SHR': (0x19, 'R'), 'SRA': (0x1A, 'R'), 'SLT': (0x1B, 'R'),
    'SLTU': (0x1C, 'R'), 'MIN': (0x1D, 'R'), 'MAX': (0x1E, 'R'), 'SEQ': (0x1F, 'R'),
    'ADDI': (0x20, 'I'), 'ANDI': (0x21, 'I'), 'ORI': (0x22, 'I'), 'XORI': (0x23, 'I'),
    'SHLI': (0x24, 'I'), 'SHRI': (0x25, 'I'), 'SRAI': (0x26, 'I'),
    'SLTI': (0x27, 'I'), 'SLTIU': (0x28, 'I'),
    'MOVI': (0x29, 'U'), 'MOV': (0x2A, 'R1'),
    'LDG': (0x30, 'LD'), 'STG': (0x31, 'ST'), 'LDS': (0x32, 'LD'),
    'STS': (0x33, 'ST'), 'LDC': (0x34, 'LDC'),
    # floating point (M15): binary32, RTZ/FTZ (see docs/ISA.md)
    'FADD': (0x35, 'R'), 'FSUB': (0x36, 'R'), 'FMUL': (0x37, 'R'),
    'FFMA': (0x38, 'R'),   # FFMA rd, rs1, rs2  ->  rd = rs1*rs2 + rd
    'FMIN': (0x39, 'R'), 'FMAX': (0x3A, 'R'),
    'I2F':  (0x3B, 'R1'), 'F2I': (0x3C, 'R1'),
}
ISETP_OP, ISETPI_OP, FSETP_OP = 0x2B, 0x2C, 0x3D
MUFU_OP, ATOMG_OP, ATOMS_OP = 0x2D, 0x3E, 0x3F
CMP = {'EQ': 0, 'NE': 1, 'LT': 2, 'LE': 3, 'GT': 4, 'GE': 5, 'LTU': 6, 'GEU': 7}
MUFU_FN = {'RCP': 0, 'RSQRT': 1, 'SIN': 2, 'COS': 3}
ATOM_FN = {'ADD': 0, 'MIN': 1, 'MAX': 2, 'EXCH': 3, 'CAS': 4}
SREG = {
    'TID_X': 0, 'TID_Y': 1, 'TID_Z': 2, 'NTID_X': 3, 'NTID_Y': 4, 'NTID_Z': 5,
    'CTAID_X': 6, 'CTAID_Y': 7, 'CTAID_Z': 8, 'NCTAID_X': 9, 'NCTAID_Y': 10,
    'NCTAID_Z': 11, 'LANEID': 12, 'WARPID': 13, 'TID_FLAT': 14,
}


class AsmError(Exception):
    pass


def enc_guard(psel, neg):
    return ((neg & 1) << 25) | ((psel & 3) << 23)


def reg(tok):
    m = re.fullmatch(r'[Rr](\d+)', tok)
    if not m or int(m.group(1)) > 15:
        raise AsmError(f"bad register '{tok}'")
    return int(m.group(1))


def preg(tok):
    m = re.fullmatch(r'[Pp](\d+)', tok)
    if not m or int(m.group(1)) > 3:
        raise AsmError(f"bad predicate '{tok}'")
    return int(m.group(1))


def imm(tok):
    tok = tok.strip().lstrip('#')
    try:
        return int(tok, 0)
    except ValueError:
        raise AsmError(f"bad immediate '{tok}'")


def imm_chk(v, bits, what):
    """Range-check a signed immediate — error instead of silent truncation."""
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    if not (lo <= v <= hi):
        raise AsmError(f"{what} immediate {v} out of range [{lo}, {hi}] "
                       f"(build wider values with MOVI/SHLI/ORI)")
    return v


def is_imm(tok):
    tok = tok.strip().lstrip('#')
    try:
        int(tok, 0)
        return True
    except ValueError:
        return False


def mem(tok):
    m = re.fullmatch(r'\[\s*[Rr](\d+)\s*(?:\+\s*(-?(?:0[xX])?[0-9a-fA-F]+))?\s*\]', tok)
    if not m:
        raise AsmError(f"bad memory operand '{tok}'")
    base = int(m.group(1))
    off = int(m.group(2), 0) if m.group(2) else 0
    return base, off


def const_idx(tok):
    m = re.fullmatch(r'[cC]\[\s*(-?(?:0[xX])?[0-9a-fA-F]+)\s*\]', tok)
    if not m:
        raise AsmError(f"bad constant operand '{tok}' (want c[idx])")
    return int(m.group(1), 0)


def split_ops(s):
    s = s.strip()
    return [p.strip() for p in s.split(',')] if s else []


def strip_comment(line):
    # ';' or '//' begins a comment
    for sep in (';', '//'):
        i = line.find(sep)
        if i >= 0:
            line = line[:i]
    return line


def parse(lines):
    """Pass 1: strip comments, collect labels -> word index, and instruction records."""
    insns = []   # (pc, guard(psel,neg), base_mnem, cmp_suffix, ops, lineno)
    labels = {}
    pc = 0
    for lineno, raw in enumerate(lines, 1):
        line = strip_comment(raw).strip()
        if not line:
            continue
        # leading labels (possibly multiple, and possibly followed by an instruction)
        while True:
            m = re.match(r'([A-Za-z_.$][\w.$]*)\s*:\s*(.*)', line)
            if not m:
                break
            lbl = m.group(1)
            if lbl in labels:
                raise AsmError(f"line {lineno}: duplicate label '{lbl}'")
            labels[lbl] = pc
            line = m.group(2).strip()
        if not line:
            continue
        # optional guard prefix @P<n> / @!P<n>
        psel, neg = 0, 0
        mg = re.match(r'@(!?)[Pp](\d+)\s+(.*)', line)
        if mg:
            neg = 1 if mg.group(1) == '!' else 0
            psel = int(mg.group(2))
            if psel > 3:
                raise AsmError(f"line {lineno}: bad guard predicate")
            line = mg.group(3).strip()
        parts = line.split(None, 1)
        mnem = parts[0]
        ops = split_ops(parts[1]) if len(parts) > 1 else []
        base, _, suffix = mnem.upper().partition('.')
        insns.append([pc, (psel, neg), base, suffix, ops, lineno])
        pc += 1
    return insns, labels, pc


def encode(rec, labels):
    pc, (psel, neg), base, suffix, ops, lineno = rec
    g = enc_guard(psel, neg)

    def op26(o):
        return (o << 26) | g

    try:
        if base == 'MUFU':
            fn = MUFU_FN.get(suffix)
            if fn is None:
                raise AsmError(f"unknown MUFU function '.{suffix}'")
            return op26(MUFU_OP) | (reg(ops[0]) << 19) | (reg(ops[1]) << 15) | fn

        if base in ('ATOMG', 'ATOMS'):
            fn = ATOM_FN.get(suffix)
            if fn is None:
                raise AsmError(f"unknown atomic function '.{suffix}'")
            b, off = mem(ops[1])
            if off != 0:
                raise AsmError("atomics take a plain [Rb] address (no offset)")
            o = ATOMG_OP if base == 'ATOMG' else ATOMS_OP
            return op26(o) | (reg(ops[0]) << 19) | (b << 15) | (reg(ops[2]) << 11) | (fn << 8)

        if base == 'ISETP' or base == 'FSETP':
            cc = CMP.get(suffix)
            if cc is None:
                raise AsmError(f"unknown compare '.{suffix}'")
            pd = preg(ops[0]); rs1 = reg(ops[1])
            if is_imm(ops[2]):
                if base == 'FSETP':
                    raise AsmError("FSETP has no immediate form")
                return op26(ISETPI_OP) | (pd << 21) | (cc << 18) | (rs1 << 14) | (imm_chk(imm(ops[2]), 14, base) & 0x3FFF)
            rs2 = reg(ops[2])
            o = FSETP_OP if base == 'FSETP' else ISETP_OP
            return op26(o) | (pd << 21) | (cc << 18) | (rs1 << 14) | (rs2 << 10)

        if base not in MNEM:
            raise AsmError(f"unknown mnemonic '{base}'")
        opc, fmt = MNEM[base]

        if fmt == 'N':
            return op26(opc)
        if fmt == 'R':
            return op26(opc) | (reg(ops[0]) << 19) | (reg(ops[1]) << 15) | (reg(ops[2]) << 11)
        if fmt == 'R1':
            return op26(opc) | (reg(ops[0]) << 19) | (reg(ops[1]) << 15)
        if fmt == 'I':
            return op26(opc) | (reg(ops[0]) << 19) | (reg(ops[1]) << 15) | (imm_chk(imm(ops[2]), 15, base) & 0x7FFF)
        if fmt == 'U':
            return op26(opc) | (reg(ops[0]) << 19) | (imm_chk(imm(ops[1]), 19, base) & 0x7FFFF)
        if fmt == 'RDSR':
            sr = SREG.get(ops[1].upper())
            if sr is None:
                sr = imm(ops[1])
            return op26(opc) | (reg(ops[0]) << 19) | (sr & 0x7FFFF)
        if fmt == 'LD':
            b, off = mem(ops[1])
            return op26(opc) | (reg(ops[0]) << 19) | (b << 15) | (off & 0x7FFF)
        if fmt == 'LDC':
            return op26(opc) | (reg(ops[0]) << 19) | (const_idx(ops[1]) & 0x7FFFF)
        if fmt == 'ST':
            b, off = mem(ops[0])
            return op26(opc) | (reg(ops[1]) << 19) | (b << 15) | (off & 0x7FFF)   # data reg in rd field
        if fmt == 'B':
            tgt = ops[0]
            if tgt not in labels:
                raise AsmError(f"undefined label '{tgt}'")
            off = labels[tgt] - pc
            return op26(opc) | (off & 0x7FFFFF)
    except AsmError:
        raise
    except (IndexError, ValueError) as e:
        raise AsmError(f"operand error: {e}")
    raise AsmError(f"unhandled format '{fmt}'")


def assemble(text):
    insns, labels, ninsn = parse(text.splitlines())
    code = []
    for rec in insns:
        try:
            code.append(encode(rec, labels))
        except AsmError as e:
            raise AsmError(f"line {rec[5]}: {e}")
    return code


def write_gpubin(path, code, kconst=None, entry=0, reg_count=16, shared_bytes=0):
    kconst = kconst or []
    with open(path, 'wb') as f:
        f.write(b'SGPU')
        f.write(struct.pack('<HH', 1, 0))                       # version, flags
        f.write(struct.pack('<IIIIII', len(code), len(kconst),  # num_insns, num_const
                            entry, reg_count, shared_bytes, 0))  # entry, reg_count, shared, reserved
        for w in code:
            f.write(struct.pack('<I', w & 0xFFFFFFFF))
        for w in kconst:
            f.write(struct.pack('<I', w & 0xFFFFFFFF))


def main(argv=None):
    ap = argparse.ArgumentParser(description="Sirion ISA assembler -> .gpubin")
    ap.add_argument('input', help="assembly source (.s)")
    ap.add_argument('-o', '--output', help="output .gpubin (default: input with .gpubin)")
    ap.add_argument('--dump', action='store_true', help="print hex encoding to stderr")
    args = ap.parse_args(argv)
    out = args.output or re.sub(r'\.[^.]*$', '', args.input) + '.gpubin'
    with open(args.input) as f:
        text = f.read()
    try:
        code = assemble(text)
    except AsmError as e:
        print(f"asm: {args.input}: {e}", file=sys.stderr)
        return 1
    write_gpubin(out, code)
    if args.dump:
        for i, w in enumerate(code):
            print(f"{i:4d}: {w:08x}", file=sys.stderr)
    print(f"asm: wrote {out} ({len(code)} instructions)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
