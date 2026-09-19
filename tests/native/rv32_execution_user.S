.section .rodata.rv32_native_probe, "a"
.option push
.option norvc
.option norelax
.align 4
.global ok_rv32_probe_begin
.global ok_rv32_probe_timer
.global ok_rv32_probe_write_fault
.global ok_rv32_probe_kernel_fault
.global ok_rv32_probe_execute_fault
.global ok_rv32_probe_float
.global ok_rv32_probe_memory
.global ok_rv32_probe_end
ok_rv32_probe_begin:
    li gp, 0x12345
    li tp, 0x67890
    li a0, 42
    li a7, 65
    ecall
    addi a0, a0, 1
    li a7, 66
    ecall
1:  j 1b
ok_rv32_probe_timer:
    addi s0, s0, 1
    j ok_rv32_probe_timer
ok_rv32_probe_write_fault:
    lui t0, 0x40000
    sw zero, 0(t0)
    ecall
ok_rv32_probe_kernel_fault:
    li t0, 0x80000000
    lw t1, 0(t0)
    ecall
ok_rv32_probe_execute_fault:
    lui t0, 0x40001
    jr t0
ok_rv32_probe_float:
    li t0, 123
    sw t0, 0(sp)
    li t0, 456
    sw t0, 4(sp)
    fld fs0, 0(sp)
    li t0, 789
    sw t0, 0(sp)
    li t0, 1023
    sw t0, 4(sp)
    fld f31, 0(sp)
    li a7, 67
    ecall
    fsd fs0, 0(sp)
    lw a0, 0(sp)
    lw a1, 4(sp)
    fsd f31, 0(sp)
    lw a2, 0(sp)
    lw a3, 4(sp)
    li a7, 68
    ecall
ok_rv32_probe_memory:
    lui t0, 0x40001
    lw a0, 0(t0)
    addi a0, a0, 1
    sw a0, 0(t0)
    li a7, 69
    ecall
    j ok_rv32_probe_memory
ok_rv32_probe_end:
.option pop
.section .note.GNU-stack,"",@progbits
