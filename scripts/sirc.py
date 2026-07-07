#!/usr/bin/env python3
"""sirc.py — a C-like kernel compiler for the Sirion GPU (M21: floats, shared memory,
barriers, atomics, SFU intrinsics, inline functions, spilling, constant folding).

High-level kernel  --sirc-->  Sirion assembly (.s)  --asm.py-->  .gpubin  --> ISS / RTL GPU.

    func float sq(float v) { return v * v; }

    kernel normalize(float* out, float* x, float* y, int n) {
        int i = tid();
        if (i < n) {
            float r = rsqrtf(sq(x[i]) + sq(y[i]));
            out[i] = x[i] * r;
        }
    }

Language:
  * types: `int`, `float`, `int*`, `float*` (params); `int`/`float` locals (initialized).
  * `__shared__ int s[N];` block-shared arrays; `barrier();` block barrier.
  * statements: assignment, array store, `if/else`, `while`, `for`, bare calls.
  * operators: `+ - *` (int+float), `/` (float only: reciprocal-multiply), `& | ^ ~ << >>`
    (int), unary `-`, comparisons `< <= > >= == !=` (int ISETP / float FSETP).
  * builtins: `tid() lane() ctaid() ntid() nctaid()`; casts `itof(e) ftoi(e)`;
    SFU `rcpf(x) rsqrtf(x) sinf(x) cosf(x)`;
    atomics `atomic_add/atomic_min/atomic_max(p, idx, val)` and
    `atomic_exch(p, idx, val)`, `atomic_cas(p, idx, cmp, val)` — return the OLD value
    (`p` is a pointer param or a `__shared__` array).
  * `func <type> name(params) { ... return expr; }` device functions, always INLINED
    (the default on real GPU compilers); recursion is a compile error.

Parameters are passed in the constant bank in declaration order (`out`->c[0], ...).
Float semantics follow the hardware: round-toward-zero, flush-to-zero (docs/ISA.md).

Codegen is a tree walk with a linear register allocator. When the 16 registers run out,
LOCAL VARIABLES spill to per-thread slots carved from the top of shared memory
(addr = SMEM_TOP - (slot+1)*tpb*4 + tid_x*4); expression temporaries must still fit.
"""
import argparse
import re
import struct
import sys

SMEM_BYTES = 16384         # matches cu_core SMEM_WORDS*4

# ---------------------------------------------------------------- lexer
KEYWORDS = {'kernel', 'func', 'int', 'float', 'if', 'else', 'while', 'for', 'return',
            '__shared__'}
TOK_RE = re.compile(r'''
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<fnum>\d+\.\d*f?|\d+f)
  | (?P<num>0[xX][0-9a-fA-F]+|\d+)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op><<|>>|<=|>=|==|!=|[-+*/&|^~<>=(){}\[\];,])
''', re.VERBOSE)


class Tok:
    def __init__(self, kind, val): self.kind, self.val = kind, val
    def __repr__(self): return f'{self.kind}:{self.val}'


def f2bits(f):
    return struct.unpack('<I', struct.pack('<f', float(f)))[0]


def lex(src):
    toks, i = [], 0
    while i < len(src):
        m = TOK_RE.match(src, i)
        if not m:
            raise SyntaxError(f'bad token at {src[i:i+16]!r}')
        i = m.end()
        if m.lastgroup in ('ws', 'lc'):
            continue
        if m.lastgroup == 'fnum':
            toks.append(Tok('fnum', f2bits(m.group().rstrip('fF'))))
        elif m.lastgroup == 'num':
            toks.append(Tok('num', int(m.group(), 0)))
        elif m.lastgroup == 'id':
            g = m.group()
            toks.append(Tok('kw' if g in KEYWORDS else 'id', g))
        else:
            toks.append(Tok('op', m.group()))
    toks.append(Tok('eof', None))
    return toks


# ---------------------------------------------------------------- parser (AST = tuples)
class Parser:
    def __init__(self, toks): self.toks, self.i = toks, 0
    def peek(self): return self.toks[self.i]
    def next(self): t = self.toks[self.i]; self.i += 1; return t
    def eat(self, kind, val=None):
        t = self.next()
        if t.kind != kind or (val is not None and t.val != val):
            raise SyntaxError(f'expected {val or kind}, got {t}')
        return t
    def accept(self, kind, val):
        t = self.peek()
        if t.kind == kind and t.val == val:
            self.i += 1; return True
        return False

    def type_name(self):
        t = self.next()
        if t.kind != 'kw' or t.val not in ('int', 'float'):
            raise SyntaxError(f'expected type, got {t}')
        base = t.val
        if self.accept('op', '*'):
            return base + '*'
        return base

    def params(self):
        self.eat('op', '(')
        ps = []
        if not (self.peek().kind == 'op' and self.peek().val == ')'):
            while True:
                ty = self.type_name()
                pname = self.eat('id').val
                ps.append((pname, ty))
                if not self.accept('op', ','):
                    break
        self.eat('op', ')')
        return ps

    def parse_unit(self):
        funcs = {}
        while self.peek().kind == 'kw' and self.peek().val == 'func':
            self.next()
            rty = self.type_name()
            name = self.eat('id').val
            ps = self.params()
            body = self.block()
            funcs[name] = (rty, ps, body)
        self.eat('kw', 'kernel')
        name = self.eat('id').val
        ps = self.params()
        body = self.block()
        self.eat('eof')
        return ('unit', funcs, name, ps, body)

    def block(self):
        self.eat('op', '{')
        stmts = []
        while not (self.peek().kind == 'op' and self.peek().val == '}'):
            stmts.append(self.stmt())
        self.eat('op', '}')
        return stmts

    def stmt(self):
        t = self.peek()
        if t.kind == 'kw' and t.val == '__shared__':
            self.next(); self.eat('kw', 'int')
            name = self.eat('id').val
            self.eat('op', '['); n = self.eat('num').val; self.eat('op', ']')
            self.eat('op', ';')
            return ('shared', name, n)
        if t.kind == 'kw' and t.val in ('int', 'float'):
            ty = self.next().val
            name = self.eat('id').val
            self.eat('op', '=')
            e = self.expr()
            self.eat('op', ';')
            return ('decl', ty, name, e)
        if t.kind == 'kw' and t.val == 'if':
            self.next(); self.eat('op', '(')
            c = self.cond(); self.eat('op', ')')
            then = self.block()
            els = self.block() if self.accept('kw', 'else') else None
            return ('if', c, then, els)
        if t.kind == 'kw' and t.val == 'while':
            self.next(); self.eat('op', '(')
            c = self.cond(); self.eat('op', ')')
            return ('while', c, self.block())
        if t.kind == 'kw' and t.val == 'for':
            self.next(); self.eat('op', '(')
            init = self.stmt_simple(); self.eat('op', ';')
            c = self.cond(); self.eat('op', ';')
            upd = self.stmt_simple(); self.eat('op', ')')
            body = self.block()
            return ('for', init, c, upd, body)
        if t.kind == 'kw' and t.val == 'return':
            self.next()
            e = self.expr(); self.eat('op', ';')
            return ('return', e)
        s = self.stmt_simple(); self.eat('op', ';')
        return s

    def stmt_simple(self):
        # declaration inside for-init
        if self.peek().kind == 'kw' and self.peek().val in ('int', 'float'):
            ty = self.next().val
            name = self.eat('id').val
            self.eat('op', '=')
            return ('decl', ty, name, self.expr())
        name = self.eat('id').val
        if self.accept('op', '('):            # bare call statement: barrier(); atomic_...
            args = self.args()
            return ('callstmt', name, args)
        if self.accept('op', '['):
            idx = self.expr(); self.eat('op', ']')
            self.eat('op', '=')
            return ('store', name, idx, self.expr())
        self.eat('op', '=')
        return ('assign', name, self.expr())

    def args(self):
        a = []
        if not (self.peek().kind == 'op' and self.peek().val == ')'):
            while True:
                a.append(self.expr())
                if not self.accept('op', ','):
                    break
        self.eat('op', ')')
        return a

    def cond(self):
        left = self.expr()
        t = self.peek()
        if t.kind == 'op' and t.val in ('<', '<=', '>', '>=', '==', '!='):
            self.next()
            return ('cmp', t.val, left, self.expr())
        return ('nz', left)

    PREC = {'|': 1, '^': 2, '&': 3, '<<': 4, '>>': 4, '+': 5, '-': 5, '*': 6, '/': 6}
    def expr(self, min_prec=1):
        left = self.unary()
        while True:
            t = self.peek()
            if t.kind == 'op' and t.val in self.PREC and self.PREC[t.val] >= min_prec:
                op = self.next().val
                right = self.expr(self.PREC[op] + 1)
                left = ('bin', op, left, right)
            else:
                return left

    def unary(self):
        t = self.peek()
        if t.kind == 'op' and t.val in ('-', '~'):
            self.next()
            return ('un', t.val, self.unary())
        return self.primary()

    def primary(self):
        t = self.next()
        if t.kind == 'num':
            return ('num', t.val)
        if t.kind == 'fnum':
            return ('fnum', t.val)
        if t.kind == 'op' and t.val == '(':
            e = self.expr(); self.eat('op', ')'); return e
        if t.kind == 'id':
            if self.accept('op', '('):
                return ('call', t.val, self.args())
            if self.accept('op', '['):
                idx = self.expr(); self.eat('op', ']')
                return ('index', t.val, idx)
            return ('var', t.val)
        raise SyntaxError(f'unexpected {t}')


# ---------------------------------------------------------------- codegen
IBIN  = {'+': 'ADD', '-': 'SUB', '*': 'MUL', '&': 'AND', '|': 'OR', '^': 'XOR',
         '<<': 'SHL', '>>': 'SHR'}
FBIN  = {'+': 'FADD', '-': 'FSUB', '*': 'FMUL'}
CMPSUF = {'<': 'LT', '<=': 'LE', '>': 'GT', '>=': 'GE', '==': 'EQ', '!=': 'NE'}
SREG  = {'tid': 'TID_FLAT', 'lane': 'LANEID', 'ctaid': 'CTAID_X',
         'ntid': 'NTID_X', 'nctaid': 'NCTAID_X'}
SFU   = {'rcpf': 'RCP', 'rsqrtf': 'RSQRT', 'sinf': 'SIN', 'cosf': 'COS'}
ATOMIC = {'atomic_add': 'ADD', 'atomic_min': 'MIN', 'atomic_max': 'MAX',
          'atomic_exch': 'EXCH', 'atomic_cas': 'CAS'}


class CompileError(Exception):
    pass


class Gen:
    def __init__(self, funcs):
        self.funcs = funcs
        self.out = []
        self.free = list(range(16))     # R0..R15
        self.vars = {}                  # name -> ('reg', r, type) | ('spill', slot, type)
        self.shared = {}                # name -> byte offset in shared memory
        self.shared_top = 0
        self.spill_slots = 0
        self.pfree = [1, 2, 3]
        self.nlabel = 0
        self.inline_depth = 0
        self.zero = self.alloc()

    def emit(self, s): self.out.append('        ' + s)
    def label(self, l): self.out.append(l + ':')
    def newlabel(self, base='L'): self.nlabel += 1; return f'{base}{self.nlabel}'

    def alloc(self):
        if not self.free:
            raise CompileError('out of registers in an expression '
                               '(simplify it or split into locals — locals can spill)')
        return self.free.pop(0)
    def free_reg(self, r): self.free.append(r); self.free.sort()
    def alloc_pred(self):
        if not self.pfree:
            raise CompileError('out of predicate registers (nesting too deep)')
        return self.pfree.pop(0)
    def free_pred(self, p): self.pfree.append(p); self.pfree.sort()

    # ---- top level ----
    def compile(self, ast):
        _, funcs, name, params, body = ast
        self.out.append(f'; kernel {name} - compiled by sirc.py')
        self.emit(f'MOVI   R{self.zero}, 0')
        for i, (pname, ty) in enumerate(params):
            r = self.alloc(); self.vars[pname] = ('reg', r, ty)
            self.emit(f'LDC    R{r}, c[{i}]')
        for s in body:
            self.gen_stmt(s)
        self.emit('EXIT')
        return '\n'.join(self.out) + '\n'

    # ---- constants ----
    def emit_const(self, r, v):
        v &= 0xFFFFFFFF
        sv = v - (1 << 32) if v & 0x80000000 else v
        if -(1 << 18) <= sv < (1 << 18):
            self.emit(f'MOVI   R{r}, {sv}')
        else:
            self.emit(f'MOVI   R{r}, {(v >> 16) & 0xFFFF}')
            self.emit(f'SHLI   R{r}, R{r}, 8')
            self.emit(f'ORI    R{r}, R{r}, {(v >> 8) & 0xFF}')
            self.emit(f'SHLI   R{r}, R{r}, 8')
            self.emit(f'ORI    R{r}, R{r}, {v & 0xFF}')

    # ---- constant folding (integers only; floats keep hardware RTZ semantics) ----
    def fold(self, e):
        if e[0] == 'bin':
            a, b = self.fold(e[2]), self.fold(e[3])
            if a[0] == 'num' and b[0] == 'num' and e[1] in IBIN:
                x, y = a[1], b[1]
                v = {'+': x+y, '-': x-y, '*': x*y, '&': x & y, '|': x | y, '^': x ^ y,
                     '<<': x << (y & 31), '>>': (x & 0xFFFFFFFF) >> (y & 31)}[e[1]]
                return ('num', v & 0xFFFFFFFF)
            return ('bin', e[1], a, b)
        if e[0] == 'un':
            a = self.fold(e[2])
            if a[0] == 'num':
                return ('num', (-a[1] if e[1] == '-' else ~a[1]) & 0xFFFFFFFF)
            return ('un', e[1], a)
        return e

    # ---- variable access (register or spill slot) ----
    def spill_addr(self, rt, slot):
        # addr = SMEM_BYTES - (slot+1)*tpb*4 + tid_x*4   (per-thread slot)
        self.emit(f'RDSR   R{rt}, NTID_X')
        self.emit(f'SHLI   R{rt}, R{rt}, 2')
        rb = self.alloc()
        self.emit_const(rb, SMEM_BYTES)
        # base - (slot+1)*tpb*4: multiply tpb*4 by (slot+1) then subtract
        rk = self.alloc()
        self.emit_const(rk, slot + 1)
        self.emit(f'MUL    R{rt}, R{rt}, R{rk}')
        self.emit(f'SUB    R{rb}, R{rb}, R{rt}')
        self.emit(f'RDSR   R{rt}, TID_X')
        self.emit(f'SHLI   R{rt}, R{rt}, 2')
        self.emit(f'ADD    R{rt}, R{rb}, R{rt}')
        self.free_reg(rb); self.free_reg(rk)

    def var_read(self, name):
        kind, loc, ty = self.vars[name]
        if kind == 'reg':
            return loc, False, ty
        rt = self.alloc()
        self.spill_addr(rt, loc)
        self.emit(f'LDS    R{rt}, [R{rt}]')
        return rt, True, ty

    def var_write(self, name, rv):
        kind, loc, _ty = self.vars[name]
        if kind == 'reg':
            self.emit(f'MOV    R{loc}, R{rv}')
        else:
            rt = self.alloc()
            self.spill_addr(rt, loc)
            self.emit(f'STS    [R{rt}], R{rv}')
            self.free_reg(rt)

    # ---- type conversion ----
    def to_float(self, r, temp, ty):
        if ty == 'float':
            return r, temp
        rr = r if temp else self.alloc()
        self.emit(f'I2F    R{rr}, R{r}')
        return rr, True
    def to_int(self, r, temp, ty):
        if ty != 'float':
            return r, temp
        rr = r if temp else self.alloc()
        self.emit(f'F2I    R{rr}, R{r}')
        return rr, True

    # ---- expressions: return (reg, is_temp, type) ----
    def gen_expr(self, e):
        e = self.fold(e)
        tag = e[0]
        if tag == 'num':
            r = self.alloc(); self.emit_const(r, e[1]); return r, True, 'int'
        if tag == 'fnum':
            r = self.alloc(); self.emit_const(r, e[1]); return r, True, 'float'
        if tag == 'var':
            if e[1] not in self.vars:
                raise CompileError(f'undefined variable {e[1]}')
            return self.var_read(e[1])
        if tag == 'call':
            return self.gen_call(e[1], e[2])
        if tag == 'un':
            rr, tr, ty = self.gen_expr(e[2]); r = self.alloc()
            if e[1] == '-':
                if ty == 'float':
                    rz = self.alloc(); self.emit_const(rz, 0)
                    self.emit(f'FSUB   R{r}, R{rz}, R{rr}')
                    self.free_reg(rz)
                else:
                    self.emit(f'SUB    R{r}, R{self.zero}, R{rr}')
            else:
                if ty == 'float':
                    raise CompileError('~ is integer-only')
                self.emit(f'NOT    R{r}, R{rr}')
            if tr: self.free_reg(rr)
            return r, True, ty
        if tag == 'bin':
            ra, ta, tya = self.gen_expr(e[2])
            rb, tb, tyb = self.gen_expr(e[3])
            fp = (tya == 'float') or (tyb == 'float')
            if fp:
                ra, ta = self.to_float(ra, ta, tya)
                rb, tb = self.to_float(rb, tb, tyb)
                r = self.alloc()
                if e[1] == '/':
                    self.emit(f'MUFU.RCP R{r}, R{rb}')       # a/b = a * (1/b)
                    self.emit(f'FMUL   R{r}, R{ra}, R{r}')
                elif e[1] in FBIN:
                    self.emit(f'{FBIN[e[1]]:<6} R{r}, R{ra}, R{rb}')
                else:
                    raise CompileError(f'operator {e[1]} is integer-only')
            else:
                if e[1] == '/':
                    raise CompileError('integer / is not in the ISA (use shifts, or floats)')
                r = self.alloc()
                self.emit(f'{IBIN[e[1]]:<6} R{r}, R{ra}, R{rb}')
            if ta: self.free_reg(ra)
            if tb: self.free_reg(rb)
            return r, True, ('float' if fp else 'int')
        if tag == 'index':
            base_ty = self.index_base_type(e[1])
            rt = self.gen_addr(e[1], e[2])
            if e[1] in self.shared:
                self.emit(f'LDS    R{rt}, [R{rt}]')
                return rt, True, 'int'
            self.emit(f'LDG    R{rt}, [R{rt}]')
            return rt, True, base_ty
        raise CompileError(f'cannot compile expression {e}')

    def index_base_type(self, name):
        if name in self.shared:
            return 'int'
        if name not in self.vars:
            raise CompileError(f'undefined pointer {name}')
        ty = self.vars[name][2]
        if not ty.endswith('*'):
            raise CompileError(f'{name} is not a pointer')
        return ty[:-1]

    # ---- calls: builtins, casts, SFU, atomics, inline device functions ----
    def gen_call(self, name, args):
        if name in SREG:
            if args: raise CompileError(f'{name}() takes no arguments')
            r = self.alloc(); self.emit(f'RDSR   R{r}, {SREG[name]}'); return r, True, 'int'
        if name in ('itof', 'ftoi'):
            if len(args) != 1: raise CompileError(f'{name}(e) takes 1 argument')
            r, t, ty = self.gen_expr(args[0])
            if name == 'itof':
                r, t = self.to_float(r, t, ty);  return r, t, 'float'
            r, t = self.to_int(r, t, ty);        return r, t, 'int'
        if name in SFU:
            if len(args) != 1: raise CompileError(f'{name}(x) takes 1 argument')
            ra, ta, ty = self.gen_expr(args[0])
            ra, ta = self.to_float(ra, ta, ty)
            r = self.alloc()
            self.emit(f'MUFU.{SFU[name]} R{r}, R{ra}')
            if ta: self.free_reg(ra)
            return r, True, 'float'
        if name in ATOMIC:
            return self.gen_atomic(name, args)
        if name == 'barrier':
            raise CompileError('barrier() is a statement, not an expression')
        if name in self.funcs:
            return self.gen_inline(name, args)
        raise CompileError(f'unknown function {name}()')

    def gen_atomic(self, name, args):
        fn = ATOMIC[name]
        want = 4 if fn == 'CAS' else 3
        if len(args) != want:
            raise CompileError(f'{name}(ptr, idx{", cmp" if fn == "CAS" else ""}, val)')
        ptr = args[0]
        if ptr[0] != 'var':
            raise CompileError(f'{name}: first argument must be a pointer or shared array')
        pname = ptr[1]
        shared = pname in self.shared
        rt = self.gen_addr(pname, args[1])
        rold = self.alloc()
        if fn == 'CAS':
            rc, tc, _ = self.gen_expr(args[2])
            self.emit(f'MOV    R{rold}, R{rc}')     # CAS compare rides in rd
            if tc: self.free_reg(rc)
            rv, tv, _ = self.gen_expr(args[3])
        else:
            rv, tv, _ = self.gen_expr(args[2])
        op = 'ATOMS' if shared else 'ATOMG'
        self.emit(f'{op}.{fn} R{rold}, [R{rt}], R{rv}')
        self.free_reg(rt)
        if tv: self.free_reg(rv)
        return rold, True, 'int'

    def gen_inline(self, name, args):
        rty, params, body = self.funcs[name]
        if len(args) != len(params):
            raise CompileError(f'{name}() expects {len(params)} arguments')
        if self.inline_depth >= 8:
            raise CompileError(f'call depth limit reached inlining {name}() (recursion?)')
        self.inline_depth += 1
        saved = dict(self.vars)
        bound = []
        for (pname, pty), a in zip(params, args):
            ra, ta, tya = self.gen_expr(a)
            if pty == 'float':
                ra, ta = self.to_float(ra, ta, tya)
            elif tya == 'float':
                ra, ta = self.to_int(ra, ta, tya)
            rp = self.alloc()
            self.emit(f'MOV    R{rp}, R{ra}')
            if ta: self.free_reg(ra)
            bound.append(rp)
            self.vars[pname] = ('reg', rp, pty)
        ret = None
        for s in body:
            if s[0] == 'return':
                rr, tr, ty = self.gen_expr(s[1])
                if rty == 'float':
                    rr, tr = self.to_float(rr, tr, ty)
                elif ty == 'float':
                    rr, tr = self.to_int(rr, tr, ty)
                if not tr:                       # copy so callee regs can be released
                    rc = self.alloc(); self.emit(f'MOV    R{rc}, R{rr}'); rr = rc
                ret = rr
                break
            self.gen_stmt(s)
        if ret is None:
            raise CompileError(f'{name}() has no return')
        # release params + any locals the inline body declared
        for pname_r in bound:
            self.free_reg(pname_r)
        for vname, v in list(self.vars.items()):
            if vname not in saved and v[0] == 'reg' and v[1] != ret:
                self.free_reg(v[1])
        self.vars = saved
        self.inline_depth -= 1
        return ret, True, rty

    def gen_addr(self, name, idx_e):
        if name in self.shared:
            ri, ti, _ = self.gen_expr(idx_e)
            rt = self.alloc()
            self.emit(f'SHLI   R{rt}, R{ri}, 2')
            if self.shared[name] != 0:
                rb = self.alloc()
                self.emit_const(rb, self.shared[name])
                self.emit(f'ADD    R{rt}, R{rt}, R{rb}')
                self.free_reg(rb)
            if ti: self.free_reg(ri)
            return rt
        if name not in self.vars:
            raise CompileError(f'undefined pointer {name}')
        rp, tp, ty = self.var_read(name)
        if not ty.endswith('*'):
            raise CompileError(f'{name} is not a pointer')
        ri, ti, _ = self.gen_expr(idx_e)
        rt = self.alloc()
        self.emit(f'SHLI   R{rt}, R{ri}, 2')
        self.emit(f'ADD    R{rt}, R{rp}, R{rt}')
        if ti: self.free_reg(ri)
        if tp: self.free_reg(rp)
        return rt

    # ---- conditions ----
    def gen_cond(self, c):
        p = self.alloc_pred()
        if c[0] == 'cmp':
            ra, ta, tya = self.gen_expr(c[2])
            rb, tb, tyb = self.gen_expr(c[3])
            if tya == 'float' or tyb == 'float':
                ra, ta = self.to_float(ra, ta, tya)
                rb, tb = self.to_float(rb, tb, tyb)
                self.emit(f'FSETP.{CMPSUF[c[1]]} P{p}, R{ra}, R{rb}')
            else:
                self.emit(f'ISETP.{CMPSUF[c[1]]} P{p}, R{ra}, R{rb}')
            if ta: self.free_reg(ra)
            if tb: self.free_reg(rb)
        else:
            r, t, _ = self.gen_expr(c[1])
            self.emit(f'ISETP.NE P{p}, R{r}, R{self.zero}')
            if t: self.free_reg(r)
        return p

    # ---- statements ----
    def gen_stmt(self, s):
        tag = s[0]
        if tag == 'shared':
            n_bytes = s[2] * 4
            if self.shared_top + n_bytes > SMEM_BYTES:
                raise CompileError('out of shared memory')
            self.shared[s[1]] = self.shared_top
            self.shared_top += n_bytes
        elif tag == 'decl':
            _, ty, name, e = s
            rv, tv, tye = self.gen_expr(e)
            if ty == 'float':
                rv, tv = self.to_float(rv, tv, tye)
            elif tye == 'float':
                rv, tv = self.to_int(rv, tv, tye)
            # keep >= 6 registers free as expression-temporary headroom; further locals
            # SPILL to per-thread shared-memory slots (correct, just slower to access)
            if len(self.free) > 6:
                r = self.alloc(); self.vars[name] = ('reg', r, ty)
                self.emit(f'MOV    R{r}, R{rv}')
            else:
                slot = self.spill_slots; self.spill_slots += 1
                if SMEM_BYTES - (slot + 1) * 256 * 4 < self.shared_top:
                    raise CompileError('out of spill space (too many locals)')
                self.vars[name] = ('spill', slot, ty)
                self.var_write(name, rv)
            if tv: self.free_reg(rv)
        elif tag == 'assign':
            if s[1] not in self.vars:
                raise CompileError(f'undefined variable {s[1]}')
            ty = self.vars[s[1]][2]
            rv, tv, tye = self.gen_expr(s[2])
            if ty == 'float':
                rv, tv = self.to_float(rv, tv, tye)
            elif tye == 'float':
                rv, tv = self.to_int(rv, tv, tye)
            self.var_write(s[1], rv)
            if tv: self.free_reg(rv)
        elif tag == 'store':
            rv, tv, tye = self.gen_expr(s[3])
            if s[1] in self.shared:
                if tye == 'float':
                    rv, tv = self.to_int(rv, tv, tye)   # shared arrays are int
                rt = self.gen_addr(s[1], s[2])
                self.emit(f'STS    [R{rt}], R{rv}')
            else:
                base_ty = self.index_base_type(s[1])
                if base_ty == 'float':
                    rv, tv = self.to_float(rv, tv, tye)
                elif tye == 'float':
                    rv, tv = self.to_int(rv, tv, tye)
                rt = self.gen_addr(s[1], s[2])
                self.emit(f'STG    [R{rt}], R{rv}')
            self.free_reg(rt)
            if tv: self.free_reg(rv)
        elif tag == 'callstmt':
            if s[1] == 'barrier':
                if s[2]: raise CompileError('barrier() takes no arguments')
                self.emit('BAR')
            elif s[1] in ATOMIC:
                r, t, _ = self.gen_atomic(s[1], s[2])
                if t: self.free_reg(r)
            elif s[1] in self.funcs:
                r, t, _ = self.gen_inline(s[1], s[2])
                if t: self.free_reg(r)
            else:
                raise CompileError(f'unknown call {s[1]}()')
        elif tag == 'if':
            self.gen_if(s[1], s[2], s[3])
        elif tag == 'while':
            self.gen_while(s[1], s[2])
        elif tag == 'for':
            self.gen_stmt(s[1])
            self.gen_while(s[2], s[4] + [s[3]])
        elif tag == 'return':
            raise CompileError('return outside a func body')
        else:
            raise CompileError(f'cannot compile statement {s}')

    def gen_if(self, cond, then, els):
        p = self.gen_cond(cond)
        Lend = self.newlabel('Lend')
        self.emit(f'SSY    {Lend}')
        if els:
            Lelse = self.newlabel('Lelse')
            self.emit(f'@!P{p} BRA    {Lelse}')
            for st in then: self.gen_stmt(st)
            self.emit(f'BRA    {Lend}')
            self.label(Lelse)
            for st in els: self.gen_stmt(st)
        else:
            self.emit(f'@!P{p} BRA    {Lend}')
            for st in then: self.gen_stmt(st)
        self.label(Lend)
        self.free_pred(p)

    def gen_while(self, cond, body):
        Lcont = self.newlabel('Lcont'); Lbrk = self.newlabel('Lbrk')
        self.label(Lcont)
        p = self.gen_cond(cond)
        self.emit(f'SSY    {Lbrk}')
        self.emit(f'@!P{p} BRA    {Lbrk}')
        for st in body: self.gen_stmt(st)
        self.emit(f'BRA    {Lcont}')
        self.label(Lbrk)
        self.free_pred(p)


def compile_src(src):
    ast = Parser(lex(src)).parse_unit()
    return Gen(ast[1]).compile(ast)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Sirion C-like kernel compiler -> assembly")
    ap.add_argument('input', help='high-level kernel (.k)')
    ap.add_argument('-o', '--output', help='output assembly (.s)')
    args = ap.parse_args(argv)
    out = args.output or re.sub(r'\.[^.]*$', '', args.input) + '.s'
    with open(args.input) as f:
        src = f.read()
    try:
        asm = compile_src(src)
    except (SyntaxError, CompileError) as e:
        print(f'sirc: {args.input}: {e}', file=sys.stderr)
        return 1
    with open(out, 'w') as f:
        f.write(asm)
    print(f'sirc: wrote {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
