
#include "ok/user/native_signal_config.hpp"
.set noreorder
.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && defined(__mips64)
    # Integer-only admission precedes CU1 installation and compiled C++.
    mfc0 $8,$16,0
    bgez $8,.Ln64_reject_platform
    nop
    mfc0 $9,$16,1
    andi $10,$9,1
    beqz $10,.Ln64_reject_fpu
    nop
    li $10,0x8000c482
    bne $8,$10,.Ln64_reject_platform
    nop
    li $10,0xfea3519b
    bne $9,$10,.Ln64_reject_platform
    nop
    # Config1.M is part of its checked exact value; Config2 exists.
    mfc0 $11,$16,2
    li $10,0x80000000
    bne $11,$10,.Ln64_reject_platform
    nop
    # Config2.M is checked before Config3; generic Config3.M must be zero.
    mfc0 $12,$16,3
    li $10,0x80
    bne $12,$10,.Ln64_reject_extensions
    nop
    mfc0 $13,$15,0
    li $10,0x00010000
    bne $13,$10,.Ln64_reject_platform
    nop
    li $14,0x24000080
    mtc0 $14,$12,0
    ehb
    mfc0 $15,$12,0
    bne $15,$14,.Ln64_reject_status
    nop
    # No CP1 access occurs before platform/FPU/FR1 readback admission.
    cfc1 $8,$0
    li $9,0x007f0000
    bne $8,$9,.Ln64_reject_fir
    nop
    ctc1 $0,$31
    ehb
    cfc1 $8,$31
    bnez $8,.Ln64_reject_fcsr
    nop
    dla $24,.Ln64_early_pass_text
    b .Ln64_early_emit
    li $15,1
.Ln64_reject_fpu:
    dla $24,.Ln64_reject_fpu_text
    b .Ln64_early_emit
    move $15,$0
.Ln64_reject_platform:
    dla $24,.Ln64_reject_platform_text
    b .Ln64_early_emit
    move $15,$0
.Ln64_reject_extensions:
    dla $24,.Ln64_reject_extensions_text
    b .Ln64_early_emit
    move $15,$0
.Ln64_reject_status:
    dla $24,.Ln64_reject_status_text
    b .Ln64_early_emit
    move $15,$0
.Ln64_reject_fir:
    dla $24,.Ln64_reject_fir_text
    b .Ln64_early_emit
    move $15,$0
.Ln64_reject_fcsr:
    dla $24,.Ln64_reject_fcsr_text
    move $15,$0
.Ln64_early_emit:
    # No stack, call, FP instruction or boot argument register is needed here.
    lui $26,0xb800
    ori $26,$26,0x3f8
.Ln64_early_emit_next:
    lbu $25,0($24)
    beqz $25,.Ln64_early_done
    nop
.Ln64_early_uart_wait:
    lbu $27,5($26)
    andi $27,$27,32
    beqz $27,.Ln64_early_uart_wait
    nop
    sb $25,0($26)
    daddiu $24,$24,1
    b .Ln64_early_emit_next
    nop
.Ln64_early_done:
    bnez $15,.Ln64_early_admitted
    nop
.Ln64_early_rejected:
    b .Ln64_early_rejected
    nop
.Ln64_early_admitted:
#endif
    # Enable n64 floating-point state before entering compiled C++.
    mfc0 $t0, $12
    li $t1, 0x24000080
    or $t0, $t0, $t1
    mtc0 $t0, $12
    ehb
    dla $sp, boot_stack_top
    dla $t0, _bss_start
    dla $t1, _bss_end
1:
    sltu $t2, $t0, $t1
    beqz $t2, 2f
    nop
    sd $zero, 0($t0)
    daddiu $t0, $t0, 8
    b 1b
    nop
2:
    daddiu $sp, $sp, -32
    jal ok_boot_capture
    nop
    jal kernel_main
    nop
3:
    b 3b
    nop

.section .bootstrap_bss, "aw", @nobits
.align 12
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:

#if defined(OK_NATIVE_SIGNAL_RUNTIME) && defined(__mips64)
.section .rodata.n64_private_early,"a"
#if defined(OK_PRIVATE_SIGNAL_DELIVERY_FIXTURE) && defined(OK_ENABLE_TEST_POINTS)
.Ln64_early_pass_text: .asciz "N64_PRIVATE_EARLY_PASS before-cxx=1 fp=fr1\n"
.Ln64_reject_fpu_text: .asciz "N64_PRIVATE_EARLY_REJECT reason=no-fpu before-cp1=1\n"
.Ln64_reject_platform_text: .asciz "N64_PRIVATE_EARLY_REJECT reason=platform before-cp1=1\n"
.Ln64_reject_extensions_text: .asciz "N64_PRIVATE_EARLY_REJECT reason=extensions before-cp1=1\n"
.Ln64_reject_status_text: .asciz "N64_PRIVATE_EARLY_REJECT reason=status before-cp1=1\n"
.Ln64_reject_fir_text: .asciz "N64_PRIVATE_EARLY_REJECT reason=fir\n"
.Ln64_reject_fcsr_text: .asciz "N64_PRIVATE_EARLY_REJECT reason=fcsr\n"
#else
.Ln64_early_pass_text: .asciz "N64_SIGNAL_PLATFORM_PASS before-cxx=1 fp=fr1\n"
.Ln64_reject_fpu_text: .asciz "N64_SIGNAL_PLATFORM_REJECT reason=no-fpu before-cp1=1\n"
.Ln64_reject_platform_text: .asciz "N64_SIGNAL_PLATFORM_REJECT reason=platform before-cp1=1\n"
.Ln64_reject_extensions_text: .asciz "N64_SIGNAL_PLATFORM_REJECT reason=extensions before-cp1=1\n"
.Ln64_reject_status_text: .asciz "N64_SIGNAL_PLATFORM_REJECT reason=status before-cp1=1\n"
.Ln64_reject_fir_text: .asciz "N64_SIGNAL_PLATFORM_REJECT reason=fir\n"
.Ln64_reject_fcsr_text: .asciz "N64_SIGNAL_PLATFORM_REJECT reason=fcsr\n"
#endif
#endif
