; vec_add — out[i] = a[i] + b[i]  for global thread i < n
; params (constant bank): c[0]=out_ptr  c[1]=a_ptr  c[2]=b_ptr  c[3]=n
        RDSR     R0, TID_FLAT        ; i = global flat thread id
        LDC      R1, c[3]            ; n
        ISETP.GE P1, R0, R1          ; P1 = (i >= n)
        SSY      Ldone
@P1     BRA      Ldone               ; out-of-range lanes skip (divergent on last warp)
        SHLI     R2, R0, 2           ; byte offset = i*4
        LDC      R3, c[1]            ; a_ptr
        LDC      R4, c[2]            ; b_ptr
        LDC      R5, c[0]            ; out_ptr
        ADD      R3, R3, R2          ; &a[i]
        ADD      R4, R4, R2          ; &b[i]
        ADD      R5, R5, R2          ; &out[i]
        LDG      R6, [R3]
        LDG      R7, [R4]
        ADD      R8, R6, R7
        STG      [R5], R8
Ldone:
        EXIT
