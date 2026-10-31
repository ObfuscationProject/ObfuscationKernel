.machine e500
.section .text
// UserFrame is 672 bytes. The scalar GPR array is canonical; SPE stores also
// preserve all 32 extended-register upper halves, ACC and SPEFSCR.
.macro SAVE_SPE base
    evstdd 0,0(\base)
    evstdd 1,8(\base)
    evstdd 2,16(\base)
    evstdd 3,24(\base)
    evstdd 4,32(\base)
    evstdd 5,40(\base)
    evstdd 6,48(\base)
    evstdd 7,56(\base)
    evstdd 8,64(\base)
    evstdd 9,72(\base)
    evstdd 10,80(\base)
    evstdd 11,88(\base)
    evstdd 12,96(\base)
    evstdd 13,104(\base)
    evstdd 14,112(\base)
    evstdd 15,120(\base)
    evstdd 16,128(\base)
    evstdd 17,136(\base)
    evstdd 18,144(\base)
    evstdd 19,152(\base)
    evstdd 20,160(\base)
    evstdd 21,168(\base)
    evstdd 22,176(\base)
    evstdd 23,184(\base)
    evstdd 24,192(\base)
    evstdd 25,200(\base)
    evstdd 26,208(\base)
    evstdd 27,216(\base)
    evstdd 28,224(\base)
    evstdd 29,232(\base)
    evstdd 30,240(\base)
    evstdd 31,248(\base)
    evxor 0,0,0
    evmwumiaa 0,0,0
    li 11,256
    evstddx 0,11,\base
    mfspr 0,512
    stw 0,264(\base)
.endm
.macro RESTORE_SPE base
    lwz 0,264(\base)
    mtspr 512,0
    li 11,256
    evlddx 0,11,\base
    evmra 0,0
    evldd 0,0(\base)
    evldd 1,8(\base)
    evldd 2,16(\base)
    evldd 3,24(\base)
    evldd 4,32(\base)
    evldd 5,40(\base)
    evldd 6,48(\base)
    evldd 7,56(\base)
    evldd 8,64(\base)
    evldd 9,72(\base)
    evldd 10,80(\base)
    evldd 11,88(\base)
    evldd 13,104(\base)
    evldd 14,112(\base)
    evldd 15,120(\base)
    evldd 16,128(\base)
    evldd 17,136(\base)
    evldd 18,144(\base)
    evldd 19,152(\base)
    evldd 20,160(\base)
    evldd 21,168(\base)
    evldd 22,176(\base)
    evldd 23,184(\base)
    evldd 24,192(\base)
    evldd 25,200(\base)
    evldd 26,208(\base)
    evldd 27,216(\base)
    evldd 28,224(\base)
    evldd 29,232(\base)
    evldd 30,240(\base)
    evldd 31,248(\base)
    evldd 12,96(\base)
.endm

.global ok_ppc_execution_enter
.type ok_ppc_execution_enter,@function
ok_ppc_execution_enter:
    mr 12,1
    cmpwi 5,0
    beq .Lservice_stack
    mr 1,5
.Lservice_stack:
    stwu 1,-752(1)
    stw 12,736(1)
    stw 12,0(1)
    stmw 0,16(1)
    mflr 0
    stw 0,144(1)
    mfctr 0
    stw 0,148(1)
    mfcr 0
    stw 0,152(1)
    mfxer 0
    stw 0,156(1)
    mfmsr 0
    stw 0,160(1)
    mfspr 11,256
    stw 11,164(1)
    wrteei 0
    li 11,0
    ori 11,11,0x8000
    andc 0,0,11
    oris 0,0,0x0200
    mtmsr 0
    isync
    mtspr 272,3
    mtspr 273,1
    mtspr 278,4
    addi 12,1,176
    SAVE_SPE 12
    mfspr 12,272
    lwz 0,132(12)
    stw 0,4(12)
    lwz 0,140(12)
    stw 0,8(12)
    lwz 0,144(12)
    stw 0,52(12)
    addi 12,12,160
    RESTORE_SPE 12
    mfspr 12,272
    lwz 0,432(12)
    mtlr 0
    lwz 0,436(12)
    mtctr 0
    lwz 0,440(12)
    mtcr 0
    lwz 0,444(12)
    mtxer 0
    lwz 0,448(12)
    mtspr 256,0
    lwz 0,128(12)
    mtspr 26,0
    lwz 0,136(12)
    mtspr 27,0
    lwz 0,0(12)
    lwz 1,4(12)
    lwz 2,8(12)
    lwz 3,12(12)
    lwz 4,16(12)
    lwz 5,20(12)
    lwz 6,24(12)
    lwz 7,28(12)
    lwz 8,32(12)
    lwz 9,36(12)
    lwz 10,40(12)
    lwz 11,44(12)
    lwz 13,52(12)
    lwz 14,56(12)
    lwz 15,60(12)
    lwz 16,64(12)
    lwz 17,68(12)
    lwz 18,72(12)
    lwz 19,76(12)
    lwz 20,80(12)
    lwz 21,84(12)
    lwz 22,88(12)
    lwz 23,92(12)
    lwz 24,96(12)
    lwz 25,100(12)
    lwz 26,104(12)
    lwz 27,108(12)
    lwz 28,112(12)
    lwz 29,116(12)
    lwz 30,120(12)
    lwz 31,124(12)
    lwz 12,48(12)
    rfi
.size ok_ppc_execution_enter,.-ok_ppc_execution_enter

// IVPR supplies the upper 16 bits; every IVOR entry is inside this 64 KiB page.
.balign 65536
.global ok_ppc_vectors
ok_ppc_vectors:
    mtspr 274,0
    li 0,0
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,1
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,2
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,3
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,4
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,5
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,6
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,7
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,8
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,9
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,10
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,11
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,12
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,13
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,14
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,15
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,16
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,17
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
    mtspr 274,0
    li 0,18
    mtspr 277,0
    b ok_ppc_trap_common
    .space 16
ok_ppc_trap_common:
    mtspr 275,12
    mfcr 0
    mtspr 276,0
    mfspr 0,27
    andi. 0,0,0x4000
    beq .Lkernel_trap
    mfspr 12,272
    stmw 0,0(12)
    mfspr 0,274
    stw 0,0(12)
    mfspr 0,275
    stw 0,48(12)
    stw 1,132(12)
    stw 2,140(12)
    stw 13,144(12)
    mfspr 0,26
    stw 0,128(12)
    mfspr 0,27
    stw 0,136(12)
    mflr 0
    stw 0,432(12)
    mfctr 0
    stw 0,436(12)
    mfspr 0,276
    stw 0,440(12)
    mfxer 0
    stw 0,444(12)
    mfspr 0,256
    stw 0,448(12)
    mfmsr 0
    oris 0,0,0x0200
    mtmsr 0
    isync
    addi 12,12,160
    SAVE_SPE 12
    mfspr 12,278
    mfspr 0,277
    stw 0,4(12)
    mfspr 0,62
    stw 0,8(12)
    mfspr 0,61
    stw 0,12(12)
    mfspr 1,273
    stw 1,16(12)
    addi 12,1,176
    RESTORE_SPE 12
    mfspr 1,273
    lwz 0,144(1)
    mtlr 0
    lwz 0,148(1)
    mtctr 0
    lwz 0,152(1)
    mtcr 0
    lwz 0,156(1)
    mtxer 0
    lwz 0,164(1)
    mtspr 256,0
    lwz 0,160(1)
    mtmsr 0
    isync
    lwz 0,16(1)
    lwz 2,24(1)
    lwz 3,28(1)
    lwz 4,32(1)
    lwz 5,36(1)
    lwz 6,40(1)
    lwz 7,44(1)
    lwz 8,48(1)
    lwz 9,52(1)
    lwz 10,56(1)
    lwz 11,60(1)
    lwz 12,64(1)
    lwz 13,68(1)
    lwz 14,72(1)
    lwz 15,76(1)
    lwz 16,80(1)
    lwz 17,84(1)
    lwz 18,88(1)
    lwz 19,92(1)
    lwz 20,96(1)
    lwz 21,100(1)
    lwz 22,104(1)
    lwz 23,108(1)
    lwz 24,112(1)
    lwz 25,116(1)
    lwz 26,120(1)
    lwz 27,124(1)
    lwz 28,128(1)
    lwz 29,132(1)
    lwz 30,136(1)
    lwz 31,140(1)
    lwz 1,736(1)
    blr

// Kernel mode: only an external input (IVOR4) or the decrementer (IVOR10)
// taken inside ok_ppc_idle's window is expected. Resume at ok_ppc_idle_end
// with MSR[EE] clear, restoring the r0, r12 and CR the stubs saved.
.Lkernel_trap:
    mfspr 0,277
    cmpwi 0,4
    beq 1f
    cmpwi 0,10
    bne ok_ppc_kernel_fault
1:  mfspr 0,26
    lis 12,ok_ppc_idle_begin@ha
    addi 12,12,ok_ppc_idle_begin@l
    cmplw 0,12
    blt ok_ppc_kernel_fault
    lis 12,ok_ppc_idle_end@ha
    addi 12,12,ok_ppc_idle_end@l
    cmplw 0,12
    bge ok_ppc_kernel_fault
    mtspr 26,12
    mfspr 0,27
    rlwinm 0,0,0,17,15
    mtspr 27,0
    mfspr 0,276
    mtcr 0
    mfspr 12,275
    mfspr 0,274
    rfi

// ok_ppc_idle(msr): runs with r3 as MSR (EE and WE set) until an interrupt is
// taken, which .Lkernel_trap turns into a return to ok_ppc_idle_end; the
// caller's MSR is then restored. Without a doze the window simply falls through.
.global ok_ppc_idle
.global ok_ppc_idle_begin
.global ok_ppc_idle_end
ok_ppc_idle:
    mfmsr 4
    sync
ok_ppc_idle_begin:
    mtmsr 3
    isync
ok_ppc_idle_end:
    mtmsr 4
    isync
    blr

ok_ppc_kernel_fault:
    // No user-frame pointer or user stack is dereferenced on a kernel fault.
    lis 12,ok_ppc_kernel_fault_message@ha
    addi 12,12,ok_ppc_kernel_fault_message@l
    lis 10,0xe000
    ori 10,10,0x4500
1:  lbz 0,0(12)
    cmpwi 0,0
    beq 3f
2:  lbz 11,5(10)
    andi. 11,11,0x20
    beq 2b
    stb 0,0(10)
    addi 12,12,1
    b 1b
3:  wrteei 0
    b 3b
.section .rodata
ok_ppc_kernel_fault_message:
    .asciz "OK_NATIVE_KERNEL_FAULT architecture=ppc\n"
.section .note.GNU-stack,"",@progbits
