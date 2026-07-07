; m3_ids — special-register coverage with a non-zero block context (run as block 2).
; Exercises TID_FLAT / CTAID_X / NTID_X etc. (M3: no memory/branches.) Checked vs ISS.
        RDSR   R1, TID_FLAT      ; = blockIdx.x*blockDim + lane
        RDSR   R2, CTAID_X       ; blockIdx.x
        RDSR   R3, LANEID
        RDSR   R4, NTID_X        ; blockDim.x
        RDSR   R5, NCTAID_X      ; gridDim.x
        ADD    R6, R1, R2
        MUL    R7, R2, R4        ; blockIdx.x * blockDim.x
        ADD    R8, R7, R3        ; == TID_FLAT (cross-check)
        SUB    R9, R1, R8        ; should be 0 on every lane
        EXIT
