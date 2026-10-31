.section .text
.option push
.option norelax
.align 2
.global ok_rv64_enter_native
.type ok_rv64_enter_native,@function
ok_rv64_enter_native:
    mv t0, sp
    beqz a3, .Lservice_stack
    mv sp, a3
.Lservice_stack:
    addi sp, sp, -384
    sd t0, 368(sp)
    sd ra, 0(sp)
    sd s0, 8(sp)
    sd s1, 16(sp)
    sd s2, 24(sp)
    sd s3, 32(sp)
    sd s4, 40(sp)
    sd s5, 48(sp)
    sd s6, 56(sp)
    sd s7, 64(sp)
    sd s8, 72(sp)
    sd s9, 80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)
    sd gp, 104(sp)
    sd tp, 112(sp)
    csrr t0, mstatus
    sd t0, 120(sp)
    csrr t0, mie
    sd t0, 128(sp)
    csrr t0, mtvec
    sd t0, 136(sp)
    csrr t0, mscratch
    sd t0, 144(sp)
    csrr t0, satp
    sd t0, 152(sp)
    csrr t0, medeleg
    sd t0, 160(sp)
    csrr t0, mideleg
    sd t0, 168(sp)
    csrr t0, mcounteren
    sd t0, 176(sp)
    csrr t0, scounteren
    sd t0, 184(sp)
    sd a0, 200(sp)
    sd a2, 208(sp)
    csrr t0, sscratch
    sd t0, 216(sp)
    ld t0, 120(sp)
    li t1, 0x6000
    and t0, t0, t1
    beqz t0, 1f
    fsd f8, 256(sp)
    fsd f9, 264(sp)
    fsd f18, 272(sp)
    fsd f19, 280(sp)
    fsd f20, 288(sp)
    fsd f21, 296(sp)
    fsd f22, 304(sp)
    fsd f23, 312(sp)
    fsd f24, 320(sp)
    fsd f25, 328(sp)
    fsd f26, 336(sp)
    fsd f27, 344(sp)
    csrr t0, fcsr
    sd t0, 352(sp)
1:
    csrw mie, zero
    csrw medeleg, zero
    csrw mideleg, zero
    li t0, 7
    csrw mcounteren, t0
    csrw scounteren, t0
    la t0, ok_rv64_native_trap
    csrw mtvec, t0
    csrw mscratch, sp
    csrw satp, a1
    sfence.vma zero, zero
    fence.i
    li t0, 0x2004000
    ld t1, 0(t0)
    sd t1, 192(sp)
    li t1, 0x200bff8
    ld t1, 0(t1)
    li t2, 100000
    add t1, t1, t2
    sd t1, 0(t0)
    la t2, ok_rv_user_deadline
    sd t1, 0(t2)
    ld t0, 120(sp)
    li t1, 0xe1808
    not t1, t1
    and t0, t0, t1
    li t1, 0x6080
    or t0, t0, t1
    csrw mstatus, t0
    la t0, ok_rv_user_mie
    ld t0, 0(t0)
    csrw mie, t0
    ld t6, 200(sp)
    ld t0, 256(t6)
    csrw mepc, t0
    fld f0, 304(t6)
    fld f1, 312(t6)
    fld f2, 320(t6)
    fld f3, 328(t6)
    fld f4, 336(t6)
    fld f5, 344(t6)
    fld f6, 352(t6)
    fld f7, 360(t6)
    fld f8, 368(t6)
    fld f9, 376(t6)
    fld f10, 384(t6)
    fld f11, 392(t6)
    fld f12, 400(t6)
    fld f13, 408(t6)
    fld f14, 416(t6)
    fld f15, 424(t6)
    fld f16, 432(t6)
    fld f17, 440(t6)
    fld f18, 448(t6)
    fld f19, 456(t6)
    fld f20, 464(t6)
    fld f21, 472(t6)
    fld f22, 480(t6)
    fld f23, 488(t6)
    fld f24, 496(t6)
    fld f25, 504(t6)
    fld f26, 512(t6)
    fld f27, 520(t6)
    fld f28, 528(t6)
    fld f29, 536(t6)
    fld f30, 544(t6)
    fld f31, 552(t6)
    ld t0, 560(t6)
    csrw fcsr, t0
    ld x1, 8(t6)
    ld x3, 24(t6)
    ld x4, 32(t6)
    ld x5, 40(t6)
    ld x6, 48(t6)
    ld x7, 56(t6)
    ld x8, 64(t6)
    ld x9, 72(t6)
    ld x10, 80(t6)
    ld x11, 88(t6)
    ld x12, 96(t6)
    ld x13, 104(t6)
    ld x14, 112(t6)
    ld x15, 120(t6)
    ld x16, 128(t6)
    ld x17, 136(t6)
    ld x18, 144(t6)
    ld x19, 152(t6)
    ld x20, 160(t6)
    ld x21, 168(t6)
    ld x22, 176(t6)
    ld x23, 184(t6)
    ld x24, 192(t6)
    ld x25, 200(t6)
    ld x26, 208(t6)
    ld x27, 216(t6)
    ld x28, 224(t6)
    ld x29, 232(t6)
    ld x30, 240(t6)
    ld sp, 264(t6)
    ld t6, 248(t6)
    mret
.align 2
ok_rv64_native_trap:
    # Preserve the only scratch GPR without touching either stack. A kernel
    # exception must never be reported as a recoverable user fault.
    csrw sscratch, t0
    csrr t0, mstatus
    srli t0, t0, 11
    andi t0, t0, 3
    bnez t0, ok_rv64_native_kernel_fault
    csrr t0, sscratch
    csrrw sp, mscratch, sp
    sd t0, 224(sp)
    sd t1, 232(sp)
    sd t2, 240(sp)
    sd t6, 248(sp)
    ld t0, 200(sp)
    sd zero, 0(t0)
    sd x1, 8(t0)
    sd x3, 24(t0)
    sd x4, 32(t0)
    sd x8, 64(t0)
    sd x9, 72(t0)
    sd x10, 80(t0)
    sd x11, 88(t0)
    sd x12, 96(t0)
    sd x13, 104(t0)
    sd x14, 112(t0)
    sd x15, 120(t0)
    sd x16, 128(t0)
    sd x17, 136(t0)
    sd x18, 144(t0)
    sd x19, 152(t0)
    sd x20, 160(t0)
    sd x21, 168(t0)
    sd x22, 176(t0)
    sd x23, 184(t0)
    sd x24, 192(t0)
    sd x25, 200(t0)
    sd x26, 208(t0)
    sd x27, 216(t0)
    sd x28, 224(t0)
    sd x29, 232(t0)
    sd x30, 240(t0)
    csrr t1, mscratch
    sd t1, 16(t0)
    sd t1, 264(t0)
    ld t1, 224(sp)
    sd t1, 40(t0)
    ld t1, 232(sp)
    sd t1, 48(t0)
    ld t1, 240(sp)
    sd t1, 56(t0)
    ld t1, 248(sp)
    sd t1, 248(t0)
    csrr t1, mepc
    sd t1, 256(t0)
    csrr t1, mstatus
    sd t1, 272(t0)
    fsd f0, 304(t0)
    fsd f1, 312(t0)
    fsd f2, 320(t0)
    fsd f3, 328(t0)
    fsd f4, 336(t0)
    fsd f5, 344(t0)
    fsd f6, 352(t0)
    fsd f7, 360(t0)
    fsd f8, 368(t0)
    fsd f9, 376(t0)
    fsd f10, 384(t0)
    fsd f11, 392(t0)
    fsd f12, 400(t0)
    fsd f13, 408(t0)
    fsd f14, 416(t0)
    fsd f15, 424(t0)
    fsd f16, 432(t0)
    fsd f17, 440(t0)
    fsd f18, 448(t0)
    fsd f19, 456(t0)
    fsd f20, 464(t0)
    fsd f21, 472(t0)
    fsd f22, 480(t0)
    fsd f23, 488(t0)
    fsd f24, 496(t0)
    fsd f25, 504(t0)
    fsd f26, 512(t0)
    fsd f27, 520(t0)
    fsd f28, 528(t0)
    fsd f29, 536(t0)
    fsd f30, 544(t0)
    fsd f31, 552(t0)
    csrr t1, fcsr
    sd t1, 560(t0)
    ld t1, 208(sp)
    csrr t2, mcause
    sw t2, 4(t1)
    sd t2, 8(t1)
    csrr t3, mtval
    sd t3, 16(t1)
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
    ld t2, 256(t0)
    addi t2, t2, 4
    sd t2, 256(t0)
    j 5f
4:
    li t3, 2
5:
    sb t3, 0(t1)
    sd sp, 24(t1)
    csrw mie, zero
    li t0, 0x2004000
    ld t1, 192(sp)
    sd t1, 0(t0)
    ld t0, 152(sp)
    csrw satp, t0
    sfence.vma zero, zero
    ld t0, 120(sp)
    li t1, 0x6000
    and t0, t0, t1
    beqz t0, 6f
    fld f8, 256(sp)
    fld f9, 264(sp)
    fld f18, 272(sp)
    fld f19, 280(sp)
    fld f20, 288(sp)
    fld f21, 296(sp)
    fld f22, 304(sp)
    fld f23, 312(sp)
    fld f24, 320(sp)
    fld f25, 328(sp)
    fld f26, 336(sp)
    fld f27, 344(sp)
    ld t0, 352(sp)
    csrw fcsr, t0
6:
    ld t0, 136(sp)
    csrw mtvec, t0
    ld t0, 144(sp)
    csrw mscratch, t0
    ld t0, 160(sp)
    csrw medeleg, t0
    ld t0, 168(sp)
    csrw mideleg, t0
    ld t0, 176(sp)
    csrw mcounteren, t0
    ld t0, 184(sp)
    csrw scounteren, t0
    ld t0, 216(sp)
    csrw sscratch, t0
    ld ra, 0(sp)
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld s2, 24(sp)
    ld s3, 32(sp)
    ld s4, 40(sp)
    ld s5, 48(sp)
    ld s6, 56(sp)
    ld s7, 64(sp)
    ld s8, 72(sp)
    ld s9, 80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    ld gp, 104(sp)
    ld tp, 112(sp)
    # Restore kernel GP/TP before an already-pending kernel IRQ can run.
    ld t0, 128(sp)
    csrw mie, t0
    ld t0, 120(sp)
    csrw mstatus, t0
    ld sp, 368(sp)
    ret
ok_rv64_native_kernel_fault:
    csrw mie, zero
    csrci mstatus, 8
    li t0, 0x10000000
    la t1, ok_rv64_kernel_fault_text
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
.size ok_rv64_enter_native, .-ok_rv64_enter_native
.section .rodata
ok_rv64_kernel_fault_text:
    .asciz "RV64_NATIVE_KERNEL_FAULT\n"
.option pop
.section .note.GNU-stack,"",@progbits
