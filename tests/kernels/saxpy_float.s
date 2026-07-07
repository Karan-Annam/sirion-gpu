; saxpy_float — out[i] = a*x[i] + y[i] in binary32 floats, via FFMA (M15).
;
; params: c[0]=out_ptr  c[1]=x_ptr  c[2]=y_ptr  c[3]=n  c[4]=a (float bits)
; The test uses exactly-representable values so the RTZ result equals real IEEE arithmetic
; (unbiased known answer), and the RTL is diffed bit-exact against the ISS.
        RDSR     R0, TID_FLAT
        LDC      R1, c[3]            ; n
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone
        SHLI     R2, R0, 2           ; byte offset
        LDC      R3, c[1]            ; x_ptr
        LDC      R4, c[2]            ; y_ptr
        LDC      R5, c[0]            ; out_ptr
        ADD      R3, R3, R2
        ADD      R4, R4, R2
        ADD      R5, R5, R2
        LDG      R6, [R3]            ; x[i]
        LDG      R7, [R4]            ; y[i]  (FFMA accumulator)
        LDC      R8, c[4]            ; a
        FFMA     R7, R8, R6          ; R7 = a*x[i] + y[i]
        STG      [R5], R7
Ldone:
        EXIT
