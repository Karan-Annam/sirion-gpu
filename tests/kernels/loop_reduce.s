; loop_reduce — out[i] = sum_{k=0}^{K-1} a[i*K + k]  for i < n  (backward-branch loop)
; params: c[0]=out_ptr  c[1]=a_ptr  c[2]=n  c[3]=K
        RDSR     R0, TID_FLAT
        LDC      R1, c[2]            ; n
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone
        LDC      R2, c[3]            ; K
        LDC      R3, c[1]            ; a base
        MUL      R4, R0, R2          ; i*K
        SHLI     R4, R4, 2           ; *4 bytes
        ADD      R3, R3, R4          ; &a[i*K]
        MOVI     R5, 0               ; sum = 0
        MOVI     R6, 0               ; k = 0
Lloop:
        ISETP.GE P2, R6, R2          ; k >= K ?
        SSY      Lend
@P2     BRA      Lend                ; uniform exit (same K on all lanes)
        LDG      R7, [R3]
        ADD      R5, R5, R7          ; sum += a[..]
        ADDI     R3, R3, 4           ; ptr += 4
        ADDI     R6, R6, 1           ; k++
        BRA      Lloop
Lend:
        LDC      R8, c[0]            ; out_ptr
        SHLI     R9, R0, 2
        ADD      R8, R8, R9
        STG      [R8], R5
Ldone:
        EXIT
