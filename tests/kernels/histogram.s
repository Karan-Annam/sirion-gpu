; histogram — hist[data[i]] += 1 via GLOBAL atomic adds (M16).
;
; Many threads across many warps and blocks increment the same bins concurrently — only a
; real atomic RMW gives the exact closed-form counts (a plain load-add-store would lose
; updates). ADD is commutative, so any warp interleaving gives the same final histogram.
;   params: c[0]=hist_ptr (bins)  c[1]=data_ptr  c[2]=n
        RDSR     R0, TID_FLAT
        LDC      R1, c[2]            ; n
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone
        SHLI     R2, R0, 2
        LDC      R3, c[1]            ; data base
        ADD      R3, R3, R2
        LDG      R4, [R3]            ; v = data[i]  (bin index)
        SHLI     R4, R4, 2
        LDC      R5, c[0]            ; hist base
        ADD      R5, R5, R4          ; &hist[v]
        MOVI     R6, 1
        ATOMG.ADD R7, [R5], R6       ; hist[v] += 1 (R7 = old count, unused)
Ldone:
        EXIT
