.section .text.boot, "ax"
.global _start
.type _start, %function
_start:
    b arm32_entry
    .space 32
    .word 0x016f2818
    .word _kernel_start
    .word _kernel_file_end
arm32_entry:
    // Retain the actual loader state before changing interrupt masks.
    mrs r8, cpsr
    mrc p15, 0, r9, c1, c0, 0
    mrc p15, 0, r10, c0, c0, 5
    // Little-endian data state is required before any stack or BSS store.
    tst r8, #0x200
    bne arm32_unsupported_entry
    tst r9, #0x02000000
    bne arm32_unsupported_entry
    mov r4, r0
    mov r5, r1
    mov r6, r2
    mov r7, r3
    cpsid if
    ldr sp, =boot_stack_top

    ldr r0, =_bss_start
    ldr r1, =_bss_end
    mov r2, #0
1:
    cmp r0, r1
    bhs 2f
    str r2, [r0], #4
    b 1b
2:
    mov r0, r4
    mov r1, r5
    mov r2, r6
    mov r3, r7
    bl ok_boot_capture
    mov r0, r8
    mov r1, r9
    mov r2, r10
    bl ok_arm32_boot_capture
    bl kernel_main
arm32_unsupported_entry:
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
