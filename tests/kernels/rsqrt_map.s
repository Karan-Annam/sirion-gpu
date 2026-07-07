; rsqrt_map — out[i] = rsqrt(in[i]) via the SFU (M16). Bit-exact vs the ISS.
;   params: c[0]=out_ptr  c[1]=in_ptr  c[2]=n
        RDSR       R0, TID_FLAT
        LDC        R1, c[2]
        ISETP.GE   P1, R0, R1
        SSY        Ldone
@P1     BRA        Ldone
        SHLI       R2, R0, 2
        LDC        R3, c[1]
        LDC        R5, c[0]
        ADD        R3, R3, R2
        ADD        R5, R5, R2
        LDG        R4, [R3]
        MUFU.RSQRT R6, R4
        STG        [R5], R6
Ldone:
        EXIT
