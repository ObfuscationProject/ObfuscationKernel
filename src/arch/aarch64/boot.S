.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    b boot_entry
    .long 0
    .quad 0x00080000
    .quad _kernel_image_size
    .quad 0x2
    .quad 0
    .quad 0
    .quad 0
    .long 0x644d5241
    .long 0

boot_entry:
    mov x20, x0
    mov x21, x1
    mov x22, x2
    mov x23, x3
    msr daifset, #0xf
    mrs x0, CurrentEL
    cmp x0, #0x8
    b.ne 0f
    // Linux Image boot permits entry at EL2. Native processes always run under
    // an EL1 kernel, with the architectural timer and AArch64 execution enabled.
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    msr cptr_el2, xzr
    ldr x0, =boot_stack_top
    msr sp_el1, x0
    adr x0, 0f
    msr elr_el2, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    eret
0:
    msr spsel, #1
    ldr x0, =boot_stack_top
    mov sp, x0

    ldr x0, =_bss_start
    ldr x1, =_bss_end
    mov x2, xzr
1:
    cmp x0, x1
    b.hs 2f
    str x2, [x0], #8
    b 1b
2:
    mov x0, x20
    mov x1, x21
    mov x2, x22
    mov x3, x23
    bl ok_boot_capture
    bl kernel_main
3:
    wfe
    b 3b

.section .bootstrap_bss, "aw", %nobits
.align 12
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:
