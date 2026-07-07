; m6_shared — shared memory + barrier. Each lane writes lane^2 to shared[lane], barriers,
; then reads shared[31-lane].  Expected R5[lane] = (31-lane)^2.
        RDSR    R1, LANEID
        MUL     R2, R1, R1        ; lane^2
        SHLI    R3, R1, 2         ; lane*4 (byte offset)
        STS     [R3], R2          ; shared[lane] = lane^2
        BAR                        ; barrier (all writes visible)
        MOVI    R4, 31
        SUB     R4, R4, R1        ; 31 - lane
        SHLI    R4, R4, 2         ; (31-lane)*4
        LDS     R5, [R4]          ; R5 = shared[31-lane] = (31-lane)^2
        EXIT
