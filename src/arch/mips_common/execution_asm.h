.set noreorder
.set noat
.section .text
#if defined(__mips64)
#define PTR 8
#define LOAD ld
#define STORE sd
#define ADD daddiu
#define ADDRESS dla
#define C0_READ dmfc0
#define C0_WRITE dmtc0
#define FP_BASE 304
#define FRAME_SIZE 816
#define USER_STATUS 0x240080b3
#else
#define PTR 4
#define LOAD lw
#define STORE sw
#define ADD addiu
#define ADDRESS la
#define C0_READ mfc0
#define C0_WRITE mtc0
#define FP_BASE 160
#define FRAME_SIZE 672
#define USER_STATUS 0x20008013
#endif
.macro SAVE_FP base
    sdc1 $f0, FP_BASE+0(\base)
#if defined(__mips64)
    sdc1 $f1, FP_BASE+8(\base)
#endif
    sdc1 $f2, FP_BASE+16(\base)
#if defined(__mips64)
    sdc1 $f3, FP_BASE+24(\base)
#endif
    sdc1 $f4, FP_BASE+32(\base)
#if defined(__mips64)
    sdc1 $f5, FP_BASE+40(\base)
#endif
    sdc1 $f6, FP_BASE+48(\base)
#if defined(__mips64)
    sdc1 $f7, FP_BASE+56(\base)
#endif
    sdc1 $f8, FP_BASE+64(\base)
#if defined(__mips64)
    sdc1 $f9, FP_BASE+72(\base)
#endif
    sdc1 $f10, FP_BASE+80(\base)
#if defined(__mips64)
    sdc1 $f11, FP_BASE+88(\base)
#endif
    sdc1 $f12, FP_BASE+96(\base)
#if defined(__mips64)
    sdc1 $f13, FP_BASE+104(\base)
#endif
    sdc1 $f14, FP_BASE+112(\base)
#if defined(__mips64)
    sdc1 $f15, FP_BASE+120(\base)
#endif
    sdc1 $f16, FP_BASE+128(\base)
#if defined(__mips64)
    sdc1 $f17, FP_BASE+136(\base)
#endif
    sdc1 $f18, FP_BASE+144(\base)
#if defined(__mips64)
    sdc1 $f19, FP_BASE+152(\base)
#endif
    sdc1 $f20, FP_BASE+160(\base)
#if defined(__mips64)
    sdc1 $f21, FP_BASE+168(\base)
#endif
    sdc1 $f22, FP_BASE+176(\base)
#if defined(__mips64)
    sdc1 $f23, FP_BASE+184(\base)
#endif
    sdc1 $f24, FP_BASE+192(\base)
#if defined(__mips64)
    sdc1 $f25, FP_BASE+200(\base)
#endif
    sdc1 $f26, FP_BASE+208(\base)
#if defined(__mips64)
    sdc1 $f27, FP_BASE+216(\base)
#endif
    sdc1 $f28, FP_BASE+224(\base)
#if defined(__mips64)
    sdc1 $f29, FP_BASE+232(\base)
#endif
    sdc1 $f30, FP_BASE+240(\base)
#if defined(__mips64)
    sdc1 $f31, FP_BASE+248(\base)
#endif
    cfc1 $27, $31
    sw $27, FP_BASE+256(\base)
    mfhi $27
    STORE $27, FP_BASE+272(\base)
    mflo $27
    STORE $27, FP_BASE+272+PTR(\base)
.endm
.macro LOAD_FP base
    ldc1 $f0, FP_BASE+0(\base)
#if defined(__mips64)
    ldc1 $f1, FP_BASE+8(\base)
#endif
    ldc1 $f2, FP_BASE+16(\base)
#if defined(__mips64)
    ldc1 $f3, FP_BASE+24(\base)
#endif
    ldc1 $f4, FP_BASE+32(\base)
#if defined(__mips64)
    ldc1 $f5, FP_BASE+40(\base)
#endif
    ldc1 $f6, FP_BASE+48(\base)
#if defined(__mips64)
    ldc1 $f7, FP_BASE+56(\base)
#endif
    ldc1 $f8, FP_BASE+64(\base)
#if defined(__mips64)
    ldc1 $f9, FP_BASE+72(\base)
#endif
    ldc1 $f10, FP_BASE+80(\base)
#if defined(__mips64)
    ldc1 $f11, FP_BASE+88(\base)
#endif
    ldc1 $f12, FP_BASE+96(\base)
#if defined(__mips64)
    ldc1 $f13, FP_BASE+104(\base)
#endif
    ldc1 $f14, FP_BASE+112(\base)
#if defined(__mips64)
    ldc1 $f15, FP_BASE+120(\base)
#endif
    ldc1 $f16, FP_BASE+128(\base)
#if defined(__mips64)
    ldc1 $f17, FP_BASE+136(\base)
#endif
    ldc1 $f18, FP_BASE+144(\base)
#if defined(__mips64)
    ldc1 $f19, FP_BASE+152(\base)
#endif
    ldc1 $f20, FP_BASE+160(\base)
#if defined(__mips64)
    ldc1 $f21, FP_BASE+168(\base)
#endif
    ldc1 $f22, FP_BASE+176(\base)
#if defined(__mips64)
    ldc1 $f23, FP_BASE+184(\base)
#endif
    ldc1 $f24, FP_BASE+192(\base)
#if defined(__mips64)
    ldc1 $f25, FP_BASE+200(\base)
#endif
    ldc1 $f26, FP_BASE+208(\base)
#if defined(__mips64)
    ldc1 $f27, FP_BASE+216(\base)
#endif
    ldc1 $f28, FP_BASE+224(\base)
#if defined(__mips64)
    ldc1 $f29, FP_BASE+232(\base)
#endif
    ldc1 $f30, FP_BASE+240(\base)
#if defined(__mips64)
    ldc1 $f31, FP_BASE+248(\base)
#endif
    lw $27, FP_BASE+256(\base)
    ctc1 $27, $31
    LOAD $27, FP_BASE+272(\base)
    mthi $27
    LOAD $27, FP_BASE+272+PTR(\base)
    mtlo $27
.endm
.global ok_mips_execution_enter
.type ok_mips_execution_enter, @function
ok_mips_execution_enter:
    ADD $29, $29, -FRAME_SIZE
    STORE $16, 16*PTR($29)
    STORE $17, 17*PTR($29)
    STORE $18, 18*PTR($29)
    STORE $19, 19*PTR($29)
    STORE $20, 20*PTR($29)
    STORE $21, 21*PTR($29)
    STORE $22, 22*PTR($29)
    STORE $23, 23*PTR($29)
    STORE $28, 28*PTR($29)
    STORE $30, 30*PTR($29)
    STORE $31, 31*PTR($29)
    C0_READ $27, $12
    STORE $27, 34*PTR($29)
    SAVE_FP $29
    ADDRESS $27, ok_mips_execution_resume_sp
    STORE $29, 0($27)
    move $26, $4
    LOAD $27, 32*PTR($26)
    C0_WRITE $27, $14
    LOAD_FP $26
    li $27, USER_STATUS
    mtc0 $27, $12
    ehb
    LOAD $1, 1*PTR($26)
    LOAD $2, 2*PTR($26)
    LOAD $3, 3*PTR($26)
    LOAD $4, 4*PTR($26)
    LOAD $5, 5*PTR($26)
    LOAD $6, 6*PTR($26)
    LOAD $7, 7*PTR($26)
    LOAD $8, 8*PTR($26)
    LOAD $9, 9*PTR($26)
    LOAD $10, 10*PTR($26)
    LOAD $11, 11*PTR($26)
    LOAD $12, 12*PTR($26)
    LOAD $13, 13*PTR($26)
    LOAD $14, 14*PTR($26)
    LOAD $15, 15*PTR($26)
    LOAD $16, 16*PTR($26)
    LOAD $17, 17*PTR($26)
    LOAD $18, 18*PTR($26)
    LOAD $19, 19*PTR($26)
    LOAD $20, 20*PTR($26)
    LOAD $21, 21*PTR($26)
    LOAD $22, 22*PTR($26)
    LOAD $23, 23*PTR($26)
    LOAD $24, 24*PTR($26)
    LOAD $25, 25*PTR($26)
    LOAD $28, 28*PTR($26)
    LOAD $30, 30*PTR($26)
    LOAD $31, 31*PTR($26)
    LOAD $29, 33*PTR($26)
    eret
    nop
.balign 4096
.global ok_mips_execution_vectors
ok_mips_execution_vectors:
    j .Luser_trap
    nop
    .org ok_mips_execution_vectors+0x80
    j .Luser_trap
    nop
    .org ok_mips_execution_vectors+0x180
    j .Luser_trap
    nop
.Luser_trap:
    mfc0 $26, $12
    andi $26, $26, 0x18
    xori $26, $26, 0x10
    bnez $26, .Lkernel_fault
    nop
    ADDRESS $26, ok_mips_execution_active_frame
    LOAD $26, 0($26)
    beqz $26, .Lkernel_fault
    nop
    STORE $1, 1*PTR($26)
    STORE $2, 2*PTR($26)
    STORE $3, 3*PTR($26)
    STORE $4, 4*PTR($26)
    STORE $5, 5*PTR($26)
    STORE $6, 6*PTR($26)
    STORE $7, 7*PTR($26)
    STORE $8, 8*PTR($26)
    STORE $9, 9*PTR($26)
    STORE $10, 10*PTR($26)
    STORE $11, 11*PTR($26)
    STORE $12, 12*PTR($26)
    STORE $13, 13*PTR($26)
    STORE $14, 14*PTR($26)
    STORE $15, 15*PTR($26)
    STORE $16, 16*PTR($26)
    STORE $17, 17*PTR($26)
    STORE $18, 18*PTR($26)
    STORE $19, 19*PTR($26)
    STORE $20, 20*PTR($26)
    STORE $21, 21*PTR($26)
    STORE $22, 22*PTR($26)
    STORE $23, 23*PTR($26)
    STORE $24, 24*PTR($26)
    STORE $25, 25*PTR($26)
    STORE $28, 28*PTR($26)
    STORE $29, 29*PTR($26)
    STORE $30, 30*PTR($26)
    STORE $31, 31*PTR($26)
    STORE $29, 33*PTR($26)
    C0_READ $27, $14
    STORE $27, 32*PTR($26)
    mfc0 $27, $12
    STORE $27, 34*PTR($26)
    SAVE_FP $26
    ADDRESS $26, ok_mips_execution_cause
    mfc0 $27, $13
    STORE $27, 0($26)
    ADDRESS $26, ok_mips_execution_bad_address
    C0_READ $27, $8
    STORE $27, 0($26)
    ADDRESS $26, ok_mips_execution_resume_sp
    LOAD $29, 0($26)
    LOAD_FP $29
    LOAD $27, 34*PTR($29)
    mtc0 $27, $12
    ehb
    LOAD $16, 16*PTR($29)
    LOAD $17, 17*PTR($29)
    LOAD $18, 18*PTR($29)
    LOAD $19, 19*PTR($29)
    LOAD $20, 20*PTR($29)
    LOAD $21, 21*PTR($29)
    LOAD $22, 22*PTR($29)
    LOAD $23, 23*PTR($29)
    LOAD $28, 28*PTR($29)
    LOAD $30, 30*PTR($29)
    LOAD $31, 31*PTR($29)
    ADD $29, $29, FRAME_SIZE
    jr $31
    nop
.Lkernel_fault:
    j ok_mips_execution_kernel_fault
    nop
.section .note.GNU-stack,"",@progbits
