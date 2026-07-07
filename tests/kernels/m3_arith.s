; m3_arith — straight-line integer ALU coverage; per-lane results derived from lane id.
; (M3: no memory, no branches.) Final registers are checked against the ISS.
        RDSR   R1, LANEID
        MOVI   R2, 3
        MUL    R2, R1, R2       ; lane*3
        ADDI   R2, R2, 7        ; +7
        SHLI   R3, R1, 2        ; lane<<2
        AND    R4, R2, R3
        OR     R5, R2, R3
        XOR    R6, R2, R3
        SUB    R7, R2, R3
        MIN    R8, R2, R3
        MAX    R9, R2, R3
        SLT    R10, R3, R2      ; (lane<<2) < (lane*3+7) ?
        NOT    R11, R1
        MOV    R12, R2
        SRAI   R13, R11, 1      ; arithmetic shift of ~lane
        EXIT
