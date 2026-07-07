; m4_abs — divergent if/else via a real branch (not just predication).
; x = lane-16 ; if (x<0) x = -x ; y = x+1.  Lanes 0..15 and 16..31 diverge, then reconverge.
        RDSR     R1, LANEID
        MOVI     R2, 16
        SUB      R1, R1, R2       ; x = lane - 16
        MOVI     R0, 0
        ISETP.GE P1, R1, R0       ; P1 = (x >= 0)
        SSY      Ldone
@P1     BRA      Ldone            ; x>=0 lanes branch (divergent)
        SUB      R1, R0, R1       ; x = -x   (x<0 lanes only)
Ldone:
        ADDI     R3, R1, 1        ; y = |x| + 1   (all lanes, reconverged)
        EXIT
