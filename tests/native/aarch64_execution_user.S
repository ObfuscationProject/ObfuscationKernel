.section .rodata.native_user,"a"
.global ok_native_user_begin, ok_native_user_end
.global ok_native_user_kernel_read, ok_native_user_readonly_write
.global ok_native_user_nx, ok_native_user_unmapped, ok_native_user_invalid
ok_native_user_begin:
    ldr x19, =0x1122334455667788
    ldr x20, =0x8877665544332211
    movi v0.16b, #0x5a
    movi v31.16b, #0xa5
    mov x0, #0x400000
    msr fpcr, x0
    mov x0, #23
    mov x1, #42
    mrs x2, tpidr_el0
    mov x8, #0x1234
    svc #0
    ldr x2, =0x10002000
    str x0, [x2]
    str q0, [x2, #16]
    str q31, [x2, #32]
    mov x8, #0x2345
    svc #0
1:
    b 1b
ok_native_user_kernel_read:
    ldr x0, =0x40080000
    ldr x1, [x0]
    udf #0
ok_native_user_readonly_write:
    mov x0, #0x10000000
    str x1, [x0]
    udf #0
ok_native_user_nx:
    ldr x0, =0x10002000
    br x0
ok_native_user_unmapped:
    mov x0, #0x30000000
    ldr x1, [x0]
    udf #0
ok_native_user_invalid:
    udf #0
.ltorg
ok_native_user_end:
.section .note.GNU-stack,"",%progbits
