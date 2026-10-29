.section .text
.option push
.option norelax
.align 2
.global ok_rv32_enter_native
.type ok_rv32_enter_native,@function
ok_rv32_enter_native:
    mv t0, sp
    beqz a3, .Lservice_stack
    mv sp, a3
.Lservice_stack:
    addi sp, sp, -256
    sw t0, 240(sp)
    sw ra, 0(sp)
    sw s0, 4(sp)
    sw s1, 8(sp)
    sw s2, 12(sp)
    sw s3, 16(sp)
    sw s4, 20(sp)
    sw s5, 24(sp)
    sw s6, 28(sp)
    sw s7, 32(sp)
    sw s8, 36(sp)
    sw s9, 40(sp)
    sw s10, 44(sp)
    sw s11, 48(sp)
    sw gp, 52(sp)
    sw tp, 56(sp)
    csrr t0, mstatus
    sw t0, 60(sp)
    csrr t0, mie
    sw t0, 64(sp)
    csrr t0, mtvec
    sw t0, 68(sp)
    csrr t0, mscratch
    sw t0, 72(sp)
    csrr t0, satp
    sw t0, 76(sp)
    csrr t0, medeleg
    sw t0, 80(sp)
    csrr t0, mideleg
    sw t0, 84(sp)
    csrr t0, mcounteren
    sw t0, 88(sp)
    csrr t0, scounteren
    sw t0, 92(sp)
    sw a0, 104(sp)
    sw a2, 108(sp)
    csrr t0, sscratch
    sw t0, 112(sp)
    lw t0, 60(sp)
    li t1, 0x6000
    and t0, t0, t1
    beqz t0, 1f
    fsd f8, 136(sp)
    fsd f9, 144(sp)
    fsd f18, 152(sp)
    fsd f19, 160(sp)
    fsd f20, 168(sp)
    fsd f21, 176(sp)
    fsd f22, 184(sp)
    fsd f23, 192(sp)
    fsd f24, 200(sp)
    fsd f25, 208(sp)
    fsd f26, 216(sp)
    fsd f27, 224(sp)
    csrr t0, fcsr
    sw t0, 232(sp)
1:
    csrw mie, zero
    csrw medeleg, zero
    csrw mideleg, zero
    li t0, 7
    csrw mcounteren, t0
    csrw scounteren, t0
    la t0, ok_rv32_native_trap
    csrw mtvec, t0
    csrw mscratch, sp
    csrw satp, a1
    sfence.vma zero, zero
    fence.i
    li t0, 0x2004000
    lw t1, 0(t0)
    lw t2, 4(t0)
    sw t1, 96(sp)
    sw t2, 100(sp)
    li t0, 0x200bff8
10:
    lw t2, 4(t0)
    lw t1, 0(t0)
    lw t3, 4(t0)
    bne t2, t3, 10b
    li t3, 100000
    add t3, t1, t3
    sltu t4, t3, t1
    add t2, t2, t4
    li t0, 0x2004000
    li t1, -1
    sw t1, 0(t0)
    sw t2, 4(t0)
    sw t3, 0(t0)
    lw t0, 60(sp)
    li t1, 0xe1808
    not t1, t1
    and t0, t0, t1
    li t1, 0x6080
    or t0, t0, t1
    csrw mstatus, t0
    la t0, ok_rv_user_mie
    lw t0, 0(t0)
    csrw mie, t0
    lw t6, 104(sp)
    lw t0, 128(t6)
    csrw mepc, t0
    fld f0, 160(t6)
    fld f1, 168(t6)
    fld f2, 176(t6)
    fld f3, 184(t6)
    fld f4, 192(t6)
    fld f5, 200(t6)
    fld f6, 208(t6)
    fld f7, 216(t6)
    fld f8, 224(t6)
    fld f9, 232(t6)
    fld f10, 240(t6)
    fld f11, 248(t6)
    fld f12, 256(t6)
    fld f13, 264(t6)
    fld f14, 272(t6)
    fld f15, 280(t6)
    fld f16, 288(t6)
    fld f17, 296(t6)
    fld f18, 304(t6)
    fld f19, 312(t6)
    fld f20, 320(t6)
    fld f21, 328(t6)
    fld f22, 336(t6)
    fld f23, 344(t6)
    fld f24, 352(t6)
    fld f25, 360(t6)
    fld f26, 368(t6)
    fld f27, 376(t6)
    fld f28, 384(t6)
    fld f29, 392(t6)
    fld f30, 400(t6)
    fld f31, 408(t6)
    lw t0, 416(t6)
    csrw fcsr, t0
    lw x1, 4(t6)
    lw x3, 12(t6)
    lw x4, 16(t6)
    lw x5, 20(t6)
    lw x6, 24(t6)
    lw x7, 28(t6)
    lw x8, 32(t6)
    lw x9, 36(t6)
    lw x10, 40(t6)
    lw x11, 44(t6)
    lw x12, 48(t6)
    lw x13, 52(t6)
    lw x14, 56(t6)
    lw x15, 60(t6)
    lw x16, 64(t6)
    lw x17, 68(t6)
    lw x18, 72(t6)
    lw x19, 76(t6)
    lw x20, 80(t6)
    lw x21, 84(t6)
    lw x22, 88(t6)
    lw x23, 92(t6)
    lw x24, 96(t6)
    lw x25, 100(t6)
    lw x26, 104(t6)
    lw x27, 108(t6)
    lw x28, 112(t6)
    lw x29, 116(t6)
    lw x30, 120(t6)
    lw sp, 132(t6)
    lw t6, 124(t6)
    mret
.align 2
ok_rv32_native_trap:
    csrw sscratch, t0
    csrr t0, mstatus
    srli t0, t0, 11
    andi t0, t0, 3
    bnez t0, ok_rv32_native_kernel_fault
    csrr t0, sscratch
    csrrw sp, mscratch, sp
    sw t0, 116(sp)
    sw t1, 120(sp)
    sw t2, 124(sp)
    sw t6, 128(sp)
    lw t0, 104(sp)
    sw zero, 0(t0)
    sw x1, 4(t0)
    sw x3, 12(t0)
    sw x4, 16(t0)
    sw x8, 32(t0)
    sw x9, 36(t0)
    sw x10, 40(t0)
    sw x11, 44(t0)
    sw x12, 48(t0)
    sw x13, 52(t0)
    sw x14, 56(t0)
    sw x15, 60(t0)
    sw x16, 64(t0)
    sw x17, 68(t0)
    sw x18, 72(t0)
    sw x19, 76(t0)
    sw x20, 80(t0)
    sw x21, 84(t0)
    sw x22, 88(t0)
    sw x23, 92(t0)
    sw x24, 96(t0)
    sw x25, 100(t0)
    sw x26, 104(t0)
    sw x27, 108(t0)
    sw x28, 112(t0)
    sw x29, 116(t0)
    sw x30, 120(t0)
    csrr t1, mscratch
    sw t1, 8(t0)
    sw t1, 132(t0)
    lw t1, 116(sp)
    sw t1, 20(t0)
    lw t1, 120(sp)
    sw t1, 24(t0)
    lw t1, 124(sp)
    sw t1, 28(t0)
    lw t1, 128(sp)
    sw t1, 124(t0)
    csrr t1, mepc
    sw t1, 128(t0)
    csrr t1, mstatus
    sw t1, 136(t0)
    fsd f0, 160(t0)
    fsd f1, 168(t0)
    fsd f2, 176(t0)
    fsd f3, 184(t0)
    fsd f4, 192(t0)
    fsd f5, 200(t0)
    fsd f6, 208(t0)
    fsd f7, 216(t0)
    fsd f8, 224(t0)
    fsd f9, 232(t0)
    fsd f10, 240(t0)
    fsd f11, 248(t0)
    fsd f12, 256(t0)
    fsd f13, 264(t0)
    fsd f14, 272(t0)
    fsd f15, 280(t0)
    fsd f16, 288(t0)
    fsd f17, 296(t0)
    fsd f18, 304(t0)
    fsd f19, 312(t0)
    fsd f20, 320(t0)
    fsd f21, 328(t0)
    fsd f22, 336(t0)
    fsd f23, 344(t0)
    fsd f24, 352(t0)
    fsd f25, 360(t0)
    fsd f26, 368(t0)
    fsd f27, 376(t0)
    fsd f28, 384(t0)
    fsd f29, 392(t0)
    fsd f30, 400(t0)
    fsd f31, 408(t0)
    csrr t1, fcsr
    sw t1, 416(t0)
    lw t1, 108(sp)
    csrr t2, mcause
    sw t2, 4(t1)
    sw t2, 8(t1)
    csrr t3, mtval
    sw t3, 12(t1)
    bltz t2, 2f
    li t3, 8
    beq t2, t3, 3f
    li t3, 12
    beq t2, t3, 4f
    li t3, 13
    beq t2, t3, 4f
    li t3, 15
    beq t2, t3, 4f
    li t3, 3
    j 5f
2:
    // Machine timer (cause 7) is the tick; any other interrupt is a device
    // line the C side claims from the PLIC (TrapKind::interrupt).
    andi t4, t2, 0xff
    li t3, 7
    beq t4, t3, 6f
    li t3, 4
    j 5f
6:
    li t3, 1
    j 5f
3:
    li t3, 0
    lw t2, 128(t0)
    addi t2, t2, 4
    sw t2, 128(t0)
    j 5f
4:
    li t3, 2
5:
    sb t3, 0(t1)
    sw sp, 16(t1)
    csrw mie, zero
    li t0, 0x2004000
    li t1, -1
    sw t1, 0(t0)
    lw t1, 100(sp)
    sw t1, 4(t0)
    lw t1, 96(sp)
    sw t1, 0(t0)
    lw t0, 76(sp)
    csrw satp, t0
    sfence.vma zero, zero
    lw t0, 60(sp)
    li t1, 0x6000
    and t0, t0, t1
    beqz t0, 6f
    fld f8, 136(sp)
    fld f9, 144(sp)
    fld f18, 152(sp)
    fld f19, 160(sp)
    fld f20, 168(sp)
    fld f21, 176(sp)
    fld f22, 184(sp)
    fld f23, 192(sp)
    fld f24, 200(sp)
    fld f25, 208(sp)
    fld f26, 216(sp)
    fld f27, 224(sp)
    lw t0, 232(sp)
    csrw fcsr, t0
6:
    lw t0, 68(sp)
    csrw mtvec, t0
    lw t0, 72(sp)
    csrw mscratch, t0
    lw t0, 80(sp)
    csrw medeleg, t0
    lw t0, 84(sp)
    csrw mideleg, t0
    lw t0, 88(sp)
    csrw mcounteren, t0
    lw t0, 92(sp)
    csrw scounteren, t0
    lw t0, 112(sp)
    csrw sscratch, t0
    lw ra, 0(sp)
    lw s0, 4(sp)
    lw s1, 8(sp)
    lw s2, 12(sp)
    lw s3, 16(sp)
    lw s4, 20(sp)
    lw s5, 24(sp)
    lw s6, 28(sp)
    lw s7, 32(sp)
    lw s8, 36(sp)
    lw s9, 40(sp)
    lw s10, 44(sp)
    lw s11, 48(sp)
    lw gp, 52(sp)
    lw tp, 56(sp)
    lw t0, 64(sp)
    csrw mie, t0
    lw t0, 60(sp)
    csrw mstatus, t0
    lw sp, 240(sp)
    ret
ok_rv32_native_kernel_fault:
    csrw mie, zero
    csrci mstatus, 8
    li t0, 0x10000000
    la t1, ok_rv32_kernel_fault_text
7:
    lbu t2, 0(t1)
    beqz t2, 8f
    sb t2, 0(t0)
    addi t1, t1, 1
    j 7b
8:
    li t0, 0x100000
    li t1, 0x3333
    sw t1, 0(t0)
9:
    wfi
    j 9b
.size ok_rv32_enter_native, .-ok_rv32_enter_native
.section .rodata
ok_rv32_kernel_fault_text:
    .asciz "RV32_NATIVE_KERNEL_FAULT\n"
.option pop
.section .note.GNU-stack,"",@progbits
