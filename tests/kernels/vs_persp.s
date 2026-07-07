; vs_persp — VERTEX SHADER as a kernel (M19): one thread per vertex.
;
; Reads model-space vertices from global memory, applies a perspective transform using the
; FP datapath (FADD/FMUL), the SFU reciprocal (MUFU.RCP = the perspective divide), and F2I,
; and writes screen-space vertices for the rasterizer. This is the M19 point: a shader stage
; is just a compute kernel on the same SIMT core.
;
; vertex-in  (6 words): x_f, y_f, z_f, rgb, u88, v88          at c[0] + i*24
; vertex-out (6 words): xi, yi, zi(depth), qi(1/w Q16), rgb, {v,u} at c[1] + i*24
; params: c[2]=n  c[3]=f(float)  c[4]=cx(float)  c[5]=cy(float)  c[6]=zoff(float)
;         c[7]=65536.0f  c[8]=256.0f
        RDSR     R0, TID_FLAT
        LDC      R1, c[2]
        ISETP.GE P1, R0, R1
        SSY      Ldone
@P1     BRA      Ldone
        MOVI     R2, 24
        MUL      R2, R0, R2          ; byte offset i*24
        LDC      R3, c[0]
        ADD      R3, R3, R2          ; &vin[i]
        LDG      R4, [R3]            ; x
        LDG      R5, [R3+4]          ; y
        LDG      R6, [R3+8]          ; z
        LDG      R7, [R3+12]         ; rgb
        LDG      R8, [R3+16]         ; u88
        LDG      R9, [R3+20]         ; v88
        LDC      R10, c[6]
        FADD     R12, R6, R10        ; t = z + zoff
        MUFU.RCP R11, R12            ; rw = 1/t   (the perspective divide)
        LDC      R10, c[3]
        FMUL     R4, R4, R10         ; x*f
        FMUL     R4, R4, R11         ; *rw
        LDC      R10, c[4]
        FADD     R4, R4, R10         ; + cx
        F2I      R4, R4              ; xi
        LDC      R10, c[3]
        FMUL     R5, R5, R10
        FMUL     R5, R5, R11
        LDC      R10, c[5]
        FADD     R5, R5, R10
        F2I      R5, R5              ; yi
        LDC      R10, c[8]
        FMUL     R12, R12, R10
        F2I      R12, R12            ; zi = int(t * 256)  (depth grows with distance)
        LDC      R10, c[7]
        FMUL     R13, R11, R10
        F2I      R13, R13            ; qi = int(rw * 65536) = 1/w in Q16
        SHLI     R9, R9, 16
        OR       R9, R9, R8          ; {v,u} packed varying
        LDC      R10, c[1]
        ADD      R10, R10, R2        ; &vout[i]
        STG      [R10],    R4
        STG      [R10+4],  R5
        STG      [R10+8],  R12
        STG      [R10+12], R13
        STG      [R10+16], R7
        STG      [R10+20], R9
Ldone:
        EXIT
