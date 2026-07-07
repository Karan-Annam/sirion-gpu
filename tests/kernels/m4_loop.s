; m4_loop — per-lane DIVERGENT loop: each lane sums k = 0..laneid.
; Trip count differs per lane, so lanes exit the loop at different iterations — a stress
; test for the reconvergence stack. Expected R2[lane] = lane*(lane+1)/2.
        RDSR     R1, LANEID       ; n = lane
        MOVI     R2, 0            ; sum = 0
        MOVI     R3, 0            ; k = 0
Lloop:
        ISETP.GT P1, R3, R1       ; P1 = (k > n)
        SSY      Lend
@P1     BRA      Lend             ; lanes with k>n exit (progressive divergence)
        ADD      R2, R2, R3        ; sum += k
        ADDI     R3, R3, 1         ; k++
        BRA      Lloop             ; uniform back-branch (continuing lanes)
Lend:
        EXIT
