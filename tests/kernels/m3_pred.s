; m3_pred — predication + ISETP/ISETPI. Low 16 lanes and high 16 lanes take
; different updates via @P / @!P guards. (M3: no branches.) Checked vs ISS.
        RDSR     R1, LANEID
        ISETP.LT P1, R1, #16      ; ISETPI: P1 = (lane < 16)
        MOVI     R3, 100
@P1     ADDI     R3, R3, 5         ; lanes<16 -> 105 ; others stay 100
        MOVI     R4, 0
@!P1    MOVI     R4, 999           ; lanes>=16 -> 999 ; others stay 0
        ISETP.GE P2, R1, #16      ; P2 = (lane >= 16)
        MOVI     R5, 7
@P2     MUL      R5, R5, R1        ; lanes>=16 -> 7*lane ; others stay 7
        EXIT
