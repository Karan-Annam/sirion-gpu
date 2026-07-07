; saxpy_int — out[i] = alpha*a[i] + b[i]  for i < n  (integer)
; params: c[0]=out_ptr  c[1]=a_ptr  c[2]=b_ptr  c[3]=n  c[4]=alpha
        RDSR     R0, TID_FLAT
        LDC      R1, c[3]            ; n
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone
        SHLI     R2, R0, 2
        LDC      R3, c[1]            ; a_ptr
        LDC      R4, c[2]            ; b_ptr
        LDC      R5, c[0]            ; out_ptr
        LDC      R9, c[4]            ; alpha
        ADD      R3, R3, R2
        ADD      R4, R4, R2
        ADD      R5, R5, R2
        LDG      R6, [R3]
        LDG      R7, [R4]
        MUL      R8, R6, R9          ; alpha*a[i]
        ADD      R8, R8, R7          ; + b[i]
        STG      [R5], R8
Ldone:
        EXIT
