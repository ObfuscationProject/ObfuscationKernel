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

// Linux Image boot and PSCI CPU_ON may enter at EL2. Native processes always
// run under an EL1 kernel, with the architectural timer and AArch64 execution
// enabled, and EL1 reads the MPIDR the firmware names. Clobbers x0; \stack
// holds the EL1 stack top.
.macro DROP_TO_EL1 stack
    mrs x0, CurrentEL
    cmp x0, #0x8
    b.ne 9f
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    msr cptr_el2, xzr
    mrs x0, mpidr_el1
    msr vmpidr_el2, x0
    msr sp_el1, \stack
    adr x0, 9f
    msr elr_el2, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    eret
9:
.endm

boot_entry:
    mov x20, x0
    mov x21, x1
    mov x22, x2
    mov x23, x3
    msr daifset, #0xf
    ldr x1, =boot_stack_top
    DROP_TO_EL1 x1
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
    // CPU 0's per-CPU block; images without the native backend have none.
    ldr x0, =ok_aarch64_cpu_blocks
    msr tpidr_el1, x0
    mov x0, x20
    mov x1, x21
    mov x2, x22
    mov x3, x23
    bl ok_boot_capture
    bl kernel_main
3:
    wfe
    b 3b

// PSCI CPU_ON entry of a secondary CPU with the MMU off; x0 is its start
// block (src/arch/aarch64/secondary.hpp), which the boot CPU cleaned to the
// point of coherency. Nothing shared is touched before this CPU's MMU and
// caches are on, and BSS is never cleared here.
.global ok_aarch64_secondary_entry
.type ok_aarch64_secondary_entry, %function
ok_aarch64_secondary_entry:
    msr daifset, #0xf
    mov x19, x0
    ldr x1, [x19, #0]
    DROP_TO_EL1 x1
    msr spsel, #1
    ldp x1, x2, [x19, #0]
    mov sp, x1
    msr tpidr_el1, x2
    ldp x1, x2, [x19, #16]
    ldp x3, x4, [x19, #32]
    ldp x5, x6, [x19, #48]
    ldp x7, x0, [x19, #64]
    msr mair_el1, x1
    msr tcr_el1, x2
    msr vbar_el1, x3
    msr cpacr_el1, x4
    msr ttbr0_el1, x5
    isb
    tlbi vmalle1
    ic iallu
    dsb nsh
    isb
    msr sctlr_el1, x6
    isb
    br x7
.size ok_aarch64_secondary_entry, . - ok_aarch64_secondary_entry

.weak ok_aarch64_cpu_blocks

.section .bootstrap_bss, "aw", %nobits
.align 12
.global ok_service_stack_lower
ok_service_stack_lower:
boot_stack:
    .skip 1048576
.global ok_service_stack_top
ok_service_stack_top:
boot_stack_top:
