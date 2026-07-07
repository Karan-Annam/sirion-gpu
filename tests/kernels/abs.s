; abs — out[i] = |a[i]|  for i < n   (nested divergence: range guard + sign branch)
; params: c[0]=out_ptr  c[1]=a_ptr  c[2]=n
        RDSR     R0, TID_FLAT
        LDC      R1, c[2]            ; n
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone               ; outer divergence: skip out-of-range lanes
        SHLI     R2, R0, 2
        LDC      R3, c[1]            ; a_ptr
        LDC      R5, c[0]            ; out_ptr
        ADD      R3, R3, R2
        ADD      R5, R5, R2
        LDG      R6, [R3]            ; x = a[i]
        MOVI     R7, 0
        ISETP.LT P2, R6, R7          ; P2 = (x < 0)
        SSY      Lstore
@P2     BRA      Lneg                ; inner divergence on sign
        MOV      R8, R6              ; x >= 0 : keep
        BRA      Lstore
Lneg:
        SUB      R8, R7, R6          ; -x
Lstore:
        STG      [R5], R8
Ldone:
        EXIT
