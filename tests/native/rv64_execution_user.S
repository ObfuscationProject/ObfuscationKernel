.section .rodata.rv64_native_probe, "a"
.option push
.option norvc
.option norelax
.align 4
.global ok_rv64_probe_begin
.global ok_rv64_probe_timer
.global ok_rv64_probe_write_fault
.global ok_rv64_probe_kernel_fault
.global ok_rv64_probe_float
.global ok_rv64_probe_memory
.global ok_rv64_probe_end
ok_rv64_probe_begin:
    li gp, 0x12345
    li tp, 0x67890
    li a0, 42
    li a7, 65
    ecall
    addi a0, a0, 1
    li a7, 66
    ecall
1:  j 1b
ok_rv64_probe_timer:
    addi s0, s0, 1
    j ok_rv64_probe_timer
ok_rv64_probe_write_fault:
    lui t0, 0x40000
    sw zero, 0(t0)
    ecall
ok_rv64_probe_kernel_fault:
    li t0, 0x80000000
    ld t1, 0(t0)
    ecall
ok_rv64_probe_float:
    li t0, 123
    fmv.d.x fs0, t0
    li a7, 67
    ecall
    fmv.x.d a0, fs0
    li a7, 68
    ecall
ok_rv64_probe_memory:
    lui t0, 0x40001
    ld a0, 0(t0)
    addi a0, a0, 1
    sd a0, 0(t0)
    li a7, 69
    ecall
    j ok_rv64_probe_memory
ok_rv64_probe_end:
.option pop
.section .note.GNU-stack,"",@progbits
