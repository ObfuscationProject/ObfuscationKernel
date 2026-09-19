.set noreorder
.section .rodata.mips_native_probe, "a"
.balign 16
.global ok_mips_probe_begin, ok_mips_probe_end, ok_mips_probe_timer
.global ok_mips_probe_readonly, ok_mips_probe_kernel, ok_mips_probe_unmapped
.global ok_mips_probe_float, ok_mips_probe_memory, ok_mips_probe_tls
.global ok_mips_probe_delay, ok_mips_probe_invalid
.global ok_mips_probe_capture, ok_mips_probe_tls_delay, ok_mips_probe_nx
ok_mips_probe_begin:
    li $28, 0x12345
    li $4, 42
    li $2, 65
    li $16, 0x123
    mthi $16
    li $16, 0x456
    mtlo $16
    syscall
    addiu $4, $2, 1
    li $2, 66
    mfhi $17
    mflo $18
    syscall
1:  b 1b
    nop
ok_mips_probe_timer:
    addiu $16, $16, 1
    b ok_mips_probe_timer
    nop
ok_mips_probe_readonly:
    lui $8, 0x4000
    sw $0, 0($8)
    syscall
ok_mips_probe_kernel:
    lui $8, 0x8000
    lw $9, 0($8)
    syscall
ok_mips_probe_unmapped:
    lui $8, 0x6000
    lw $9, 0($8)
    syscall
ok_mips_probe_float:
    li $8, 123
    mtc1 $8, $f20
    li $2, 67
    syscall
    mfc1 $4, $f20
    li $2, 68
    syscall
ok_mips_probe_memory:
    lui $8, 0x4000
    lw $4, 4096($8)
    addiu $4, $4, 1
    sw $4, 4096($8)
    li $2, 69
    syscall
    b ok_mips_probe_memory
    nop
ok_mips_probe_tls:
    rdhwr $4, $29
    li $2, 70
    syscall
    b ok_mips_probe_tls
    nop
ok_mips_probe_delay:
    li $2, 71
    b 1f
    syscall
    break
1:  li $2, 72
    syscall
ok_mips_probe_invalid:
    break
ok_mips_probe_capture:
    syscall
    b ok_mips_probe_capture
    nop
ok_mips_probe_tls_delay:
    b 1f
    rdhwr $4, $29
    break
1:  li $2, 73
    syscall
ok_mips_probe_nx:
    lui $8, 0x4000
    addiu $8, $8, 4096
    jr $8
    nop
ok_mips_probe_end:
.section .note.GNU-stack,"",@progbits
