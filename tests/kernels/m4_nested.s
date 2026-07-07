; m4_nested — nested divergence. Outer split on lane<16; inner split on even/odd.
; Expected R5:  lane>=16 -> 1000 ; even lane<16 -> 1101 ; odd lane<16 -> 1201.
        RDSR     R1, LANEID
        MOVI     R0, 0
        MOVI     R5, 0
        ISETP.LT P1, R1, #16
        SSY      Louter
@!P1    BRA      Louter           ; lanes>=16 skip the inner block (outer divergence)
        ANDI     R6, R1, 1        ; lane & 1
        ISETP.NE P2, R6, R0       ; P2 = odd
        SSY      Linner
@P2     BRA      Lodd             ; inner divergence on parity
        MOVI     R5, 100          ; even lanes<16
        BRA      Linner
Lodd:
        MOVI     R5, 200          ; odd lanes<16
Linner:
        ADDI     R5, R5, 1        ; +1 (lanes<16 reconverged)
Louter:
        ADDI     R5, R5, 1000     ; +1000 (ALL lanes reconverged)
        EXIT
