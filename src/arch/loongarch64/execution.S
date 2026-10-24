.section .text
.macro SAVE_FP base
    fst.d $f0, \base, 0
    fst.d $f1, \base, 8
    fst.d $f2, \base, 16
    fst.d $f3, \base, 24
    fst.d $f4, \base, 32
    fst.d $f5, \base, 40
    fst.d $f6, \base, 48
    fst.d $f7, \base, 56
    fst.d $f8, \base, 64
    fst.d $f9, \base, 72
    fst.d $f10, \base, 80
    fst.d $f11, \base, 88
    fst.d $f12, \base, 96
    fst.d $f13, \base, 104
    fst.d $f14, \base, 112
    fst.d $f15, \base, 120
    fst.d $f16, \base, 128
    fst.d $f17, \base, 136
    fst.d $f18, \base, 144
    fst.d $f19, \base, 152
    fst.d $f20, \base, 160
    fst.d $f21, \base, 168
    fst.d $f22, \base, 176
    fst.d $f23, \base, 184
    fst.d $f24, \base, 192
    fst.d $f25, \base, 200
    fst.d $f26, \base, 208
    fst.d $f27, \base, 216
    fst.d $f28, \base, 224
    fst.d $f29, \base, 232
    fst.d $f30, \base, 240
    fst.d $f31, \base, 248
    movfcsr2gr $t1, $fcsr0
    st.w $t1, \base, 256
    movcf2gr $t1, $fcc0
    st.b $t1, \base, 264
    movcf2gr $t1, $fcc1
    st.b $t1, \base, 265
    movcf2gr $t1, $fcc2
    st.b $t1, \base, 266
    movcf2gr $t1, $fcc3
    st.b $t1, \base, 267
    movcf2gr $t1, $fcc4
    st.b $t1, \base, 268
    movcf2gr $t1, $fcc5
    st.b $t1, \base, 269
    movcf2gr $t1, $fcc6
    st.b $t1, \base, 270
    movcf2gr $t1, $fcc7
    st.b $t1, \base, 271
.endm
.macro LOAD_FP base
    fld.d $f0, \base, 0
    fld.d $f1, \base, 8
    fld.d $f2, \base, 16
    fld.d $f3, \base, 24
    fld.d $f4, \base, 32
    fld.d $f5, \base, 40
    fld.d $f6, \base, 48
    fld.d $f7, \base, 56
    fld.d $f8, \base, 64
    fld.d $f9, \base, 72
    fld.d $f10, \base, 80
    fld.d $f11, \base, 88
    fld.d $f12, \base, 96
    fld.d $f13, \base, 104
    fld.d $f14, \base, 112
    fld.d $f15, \base, 120
    fld.d $f16, \base, 128
    fld.d $f17, \base, 136
    fld.d $f18, \base, 144
    fld.d $f19, \base, 152
    fld.d $f20, \base, 160
    fld.d $f21, \base, 168
    fld.d $f22, \base, 176
    fld.d $f23, \base, 184
    fld.d $f24, \base, 192
    fld.d $f25, \base, 200
    fld.d $f26, \base, 208
    fld.d $f27, \base, 216
    fld.d $f28, \base, 224
    fld.d $f29, \base, 232
    fld.d $f30, \base, 240
    fld.d $f31, \base, 248
    ld.w $t1, \base, 256
    movgr2fcsr $fcsr0, $t1
    ld.bu $t1, \base, 264
    movgr2cf $fcc0, $t1
    ld.bu $t1, \base, 265
    movgr2cf $fcc1, $t1
    ld.bu $t1, \base, 266
    movgr2cf $fcc2, $t1
    ld.bu $t1, \base, 267
    movgr2cf $fcc3, $t1
    ld.bu $t1, \base, 268
    movgr2cf $fcc4, $t1
    ld.bu $t1, \base, 269
    movgr2cf $fcc5, $t1
    ld.bu $t1, \base, 270
    movgr2cf $fcc6, $t1
    ld.bu $t1, \base, 271
    movgr2cf $fcc7, $t1
.endm
.global ok_loong_execution_enter
ok_loong_execution_enter:
    move $t0, $sp
    beqz $a1, .Lservice_stack
    move $sp, $a1
.Lservice_stack:
    addi.d $sp, $sp, -432
    st.d $t0, $sp, 416
    st.d $r1, $sp, 0
    st.d $r2, $sp, 8
    st.d $r22, $sp, 16
    st.d $r23, $sp, 24
    st.d $r24, $sp, 32
    st.d $r25, $sp, 40
    st.d $r26, $sp, 48
    st.d $r27, $sp, 56
    st.d $r28, $sp, 64
    st.d $r29, $sp, 72
    st.d $r30, $sp, 80
    st.d $r31, $sp, 88
    csrrd $t0, 0x30
    st.d $t0, $sp, 96
    csrrd $t0, 0x31
    st.d $t0, $sp, 104
    move $t0, $a0
    addi.d $t2, $sp, 128
    SAVE_FP $t2
    la.local $t1, ok_loong_execution_resume_sp
    st.d $sp, $t1, 0
    csrwr $t0, 0x30
    csrrd $t0, 0x30
    ld.d $t1, $t0, 256
    csrwr $t1, 6
    li.d $t1, 7
    csrwr $t1, 1
    addi.d $t2, $t0, 304
    LOAD_FP $t2
    li.d $t1, 0xb0
    csrwr $t1, 0
    ld.d $r1, $t0, 8
    ld.d $r2, $t0, 16
    ld.d $r3, $t0, 264
    ld.d $r4, $t0, 32
    ld.d $r5, $t0, 40
    ld.d $r6, $t0, 48
    ld.d $r7, $t0, 56
    ld.d $r8, $t0, 64
    ld.d $r9, $t0, 72
    ld.d $r10, $t0, 80
    ld.d $r11, $t0, 88
    ld.d $r13, $t0, 104
    ld.d $r14, $t0, 112
    ld.d $r15, $t0, 120
    ld.d $r16, $t0, 128
    ld.d $r17, $t0, 136
    ld.d $r18, $t0, 144
    ld.d $r19, $t0, 152
    ld.d $r20, $t0, 160
    ld.d $r21, $t0, 168
    ld.d $r22, $t0, 176
    ld.d $r23, $t0, 184
    ld.d $r24, $t0, 192
    ld.d $r25, $t0, 200
    ld.d $r26, $t0, 208
    ld.d $r27, $t0, 216
    ld.d $r28, $t0, 224
    ld.d $r29, $t0, 232
    ld.d $r30, $t0, 240
    ld.d $r31, $t0, 248
    ld.d $t0, $t0, 96
    ertn
.align 12
.global ok_loong_execution_vector
ok_loong_execution_vector:
    // A PLV0 exception must not dereference a stale KS0 user-frame pointer.
    // KS1 preserves the scratch register before either stack is accessed.
    csrwr $t0, 0x31
    csrrd $t0, 1
    andi $t0, $t0, 3
    addi.d $t0, $t0, -3
    bnez $t0, .Lkernel_fault
    csrrd $t0, 0x31
    csrwr $t0, 0x30
    st.d $r1, $t0, 8
    st.d $r2, $t0, 16
    st.d $r3, $t0, 24
    st.d $r4, $t0, 32
    st.d $r5, $t0, 40
    st.d $r6, $t0, 48
    st.d $r7, $t0, 56
    st.d $r8, $t0, 64
    st.d $r9, $t0, 72
    st.d $r10, $t0, 80
    st.d $r11, $t0, 88
    st.d $r13, $t0, 104
    st.d $r14, $t0, 112
    st.d $r15, $t0, 120
    st.d $r16, $t0, 128
    st.d $r17, $t0, 136
    st.d $r18, $t0, 144
    st.d $r19, $t0, 152
    st.d $r20, $t0, 160
    st.d $r21, $t0, 168
    st.d $r22, $t0, 176
    st.d $r23, $t0, 184
    st.d $r24, $t0, 192
    st.d $r25, $t0, 200
    st.d $r26, $t0, 208
    st.d $r27, $t0, 216
    st.d $r28, $t0, 224
    st.d $r29, $t0, 232
    st.d $r30, $t0, 240
    st.d $r31, $t0, 248
    csrrd $t1, 0x30
    st.d $t1, $t0, 96
    move $s0, $zero
    b .Lsave_common
.align 12
.global ok_loong_execution_refill
ok_loong_execution_refill:
    csrwr $t0, 0x31
    csrrd $t0, 0x8f
    andi $t0, $t0, 3
    addi.d $t0, $t0, -3
    bnez $t0, .Lkernel_fault
    csrrd $t0, 0x31
    csrwr $t0, 0x30
    st.d $r1, $t0, 8
    st.d $r2, $t0, 16
    st.d $r3, $t0, 24
    st.d $r4, $t0, 32
    st.d $r5, $t0, 40
    st.d $r6, $t0, 48
    st.d $r7, $t0, 56
    st.d $r8, $t0, 64
    st.d $r9, $t0, 72
    st.d $r10, $t0, 80
    st.d $r11, $t0, 88
    st.d $r13, $t0, 104
    st.d $r14, $t0, 112
    st.d $r15, $t0, 120
    st.d $r16, $t0, 128
    st.d $r17, $t0, 136
    st.d $r18, $t0, 144
    st.d $r19, $t0, 152
    st.d $r20, $t0, 160
    st.d $r21, $t0, 168
    st.d $r22, $t0, 176
    st.d $r23, $t0, 184
    st.d $r24, $t0, 192
    st.d $r25, $t0, 200
    st.d $r26, $t0, 208
    st.d $r27, $t0, 216
    st.d $r28, $t0, 224
    st.d $r29, $t0, 232
    st.d $r30, $t0, 240
    st.d $r31, $t0, 248
    csrrd $t1, 0x30
    st.d $t1, $t0, 96
    li.d $s0, 1
.Lsave_common:
    addi.d $t2, $t0, 304
    SAVE_FP $t2
    li.d $t1, 8
    csrwr $t1, 0
    la.local $t1, ok_loong_execution_resume_sp
    ld.d $sp, $t1, 0
    // Compiled kernel code must see kernel TLS and FP control state, even if
    // the user changed rounding mode, exception enables or its thread pointer.
    ld.d $r2, $sp, 8
    addi.d $t2, $sp, 128
    LOAD_FP $t2
    move $a0, $s0
    bl ok_loong_execution_trap
    addi.d $t2, $sp, 128
    LOAD_FP $t2
    ld.d $r1, $sp, 0
    ld.d $r2, $sp, 8
    ld.d $r22, $sp, 16
    ld.d $r23, $sp, 24
    ld.d $r24, $sp, 32
    ld.d $r25, $sp, 40
    ld.d $r26, $sp, 48
    ld.d $r27, $sp, 56
    ld.d $r28, $sp, 64
    ld.d $r29, $sp, 72
    ld.d $r30, $sp, 80
    ld.d $r31, $sp, 88
    ld.d $t0, $sp, 96
    csrwr $t0, 0x30
    ld.d $t0, $sp, 104
    csrwr $t0, 0x31
    ld.d $sp, $sp, 416
    jr $ra

.Lkernel_fault:
    // No stack or user-frame pointer is trustworthy on this path. Switch to
    // direct PLV0 addressing and report the fault without compiled callbacks.
    li.d $t0, 8
    csrwr $t0, 0
    li.d $t0, 0x1fe001e0
    la.local $t1, .Lkernel_fault_text
1:
    ld.bu $t2, $t1, 0
    beqz $t2, 3f
2:
    ld.bu $t3, $t0, 5
    andi $t3, $t3, 0x20
    beqz $t3, 2b
    st.b $t2, $t0, 0
    addi.d $t1, $t1, 1
    b 1b
3:
    idle 0
    b 3b

.section .rodata
.Lkernel_fault_text:
    .asciz "OK_NATIVE_KERNEL_FAULT loongarch64\n"
.section .note.GNU-stack,"",@progbits
