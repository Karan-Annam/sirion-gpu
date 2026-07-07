; fs_texmod — FRAGMENT SHADER as a kernel (M19): one thread per fragment.
;
; Reads a rasterizer-emitted fragment record, samples the texture with ORDINARY global
; loads (nearest: no special TEX instruction needed — the GPGPU machine is the texture
; unit), modulates by the interpolated vertex color, and writes the final color back into
; the record for the ROP pass.
;
; fragment record (3 words at c[0] + i*12): w0={z,pidx}  w1={v16,u16}  w2=varying rgb
; params: c[1]=nfrags  c[2]=tex_ptr (TW=32, 32x32 RGB words)
        RDSR     R0, TID_FLAT
        LDC      R1, c[1]
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone
        MOVI     R2, 12
        MUL      R2, R0, R2
        LDC      R3, c[0]
        ADD      R3, R3, R2          ; &frag[i]
        LDG      R4, [R3+4]          ; w1 = {v,u} (8.8 texels)
        LDG      R5, [R3+8]          ; w2 = vertex-color varying
        SHLI     R6, R4, 16
        SHRI     R6, R6, 24          ; tx = u >> 8
        SHRI     R7, R4, 24          ; ty = v >> 8
        MOVI     R13, 31
        MIN      R6, R6, R13         ; clamp addressing (u/v may hit 32.0 at quad edges)
        MIN      R7, R7, R13
        SHLI     R7, R7, 5           ; * TW (32)
        ADD      R7, R7, R6
        SHLI     R7, R7, 2           ; word -> byte
        LDC      R8, c[2]
        ADD      R8, R8, R7
        LDG      R8, [R8]            ; texel (nearest sample via plain LDG)
        SHLI     R9, R8, 8
        SHRI     R9, R9, 24          ; tr
        SHLI     R10, R5, 8
        SHRI     R10, R10, 24        ; vr
        MUL      R9, R9, R10
        SHRI     R9, R9, 8           ; r' = tr*vr >> 8
        SHLI     R10, R8, 16
        SHRI     R10, R10, 24        ; tg
        SHLI     R11, R5, 16
        SHRI     R11, R11, 24        ; vg
        MUL      R10, R10, R11
        SHRI     R10, R10, 8         ; g'
        ANDI     R11, R8, 255        ; tb
        ANDI     R12, R5, 255        ; vb
        MUL      R11, R11, R12
        SHRI     R11, R11, 8         ; b'
        SHLI     R9, R9, 16
        SHLI     R10, R10, 8
        OR       R9, R9, R10
        OR       R9, R9, R11         ; packed final color
        STG      [R3+8], R9          ; shade the record in place
Ldone:
        EXIT
