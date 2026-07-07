; block_shuffle — cross-warp shared-memory shuffle (needs a REAL barrier).
;
; Every thread writes its global flat id to shared[tid_local], barriers, then reads its MIRROR
; slot shared[tpb-1-tid_local] (written by a thread in a DIFFERENT warp) and stores it to
; out[gid]. Because every thread reads another warp's write, the result is wrong under a fake
; barrier no matter which warp the scheduler runs first.
;   params: c[0] = out_ptr (indexed by global flat id)
;   expected out[b*tpb + t] = b*tpb + (tpb-1-t)     (mirror thread's global flat id)
        RDSR     R1, TID_X           ; tid_local in block
        RDSR     R10, TID_FLAT       ; gid = global flat id (value + output index)
        SHLI     R2, R1, 2           ; tid_local*4
        STS      [R2], R10           ; shared[tid_local] = gid
        BAR                          ; barrier: all warps' writes now visible
        RDSR     R3, NTID_X          ; tpb
        SUB      R4, R3, R1          ; tpb - tid_local
        ADDI     R4, R4, -1          ; tpb-1-tid_local  (mirror slot index)
        SHLI     R5, R4, 2           ; *4
        LDS      R6, [R5]            ; partner = shared[tpb-1-tid_local]
        SHLI     R7, R10, 2          ; gid*4  (output byte offset)
        LDC      R8, c[0]            ; out base
        ADD      R8, R8, R7          ; &out[gid]
        STG      [R8], R6            ; out[gid] = partner
        EXIT
