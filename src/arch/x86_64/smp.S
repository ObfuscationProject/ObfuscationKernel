// Secondary-CPU start trampoline. smp.cpp copies it to the low page at
// TRAMPOLINE_BASE and fills the parameter block; a SIPI starts the CPU in
// real mode at CS:IP = TRAMPOLINE_BASE >> 4 : 0. The code runs only from that
// copy, so every address below is an absolute low-memory address.
.set TRAMPOLINE_BASE, 0x8000
#define ABS(label) (label - ok_x86_smp_trampoline + TRAMPOLINE_BASE)

.section .rodata
.align 16
.global ok_x86_smp_trampoline
ok_x86_smp_trampoline:
.code16
    cli
    cld
    xor %ax, %ax
    mov %ax, %ds
    // Tells the starting CPU that the first SIPI was taken.
    movl $1, ABS(trampoline_arrived)
    lgdtl ABS(trampoline_gdtr)
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0
    ljmpl $0x18, $ABS(trampoline_protected)

.code32
trampoline_protected:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    // The boot CPU's CR4 (PAE, OSFXSR, OSXMMEXCPT) and its boot PML4.
    mov ABS(trampoline_cr4), %eax
    mov %eax, %cr4
    mov ABS(trampoline_cr3), %eax
    mov %eax, %cr3
    mov $0xc0000080, %ecx
    rdmsr
    or $0x901, %eax // EFER.LME, NXE and SCE.
    wrmsr
    // The boot CPU's CR0 sets PG, WP and its FPU bits, and clears the CD and
    // NW bits INIT leaves set.
    mov ABS(trampoline_cr0), %eax
    mov %eax, %cr0
    ljmp $0x08, $ABS(trampoline_long)

.code64
trampoline_long:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    // The block names one CPU at a time, and the boot CPU rewrites it for the
    // next one after giving up on this one. Read the stack and CPU first and
    // the APIC id (stored + 1, written before them) last: an id that is this
    // CPU's own vouches for the two earlier reads, any other halts here.
    mov ABS(trampoline_stack), %r8
    mov ABS(trampoline_cpu), %r9
    mov ABS(trampoline_apic_id), %r10
    mov $1, %eax
    cpuid
    shr $24, %ebx
    inc %rbx
    cmp %r10, %rbx
    jne 1f
    mov %r8, %rsp
    mov %r9d, %edi
    mov ABS(trampoline_entry), %rax
    xor %ebp, %ebp
    call *%rax
1:
    cli
    hlt
    jmp 1b

.align 8
trampoline_gdt:
    .quad 0
    .quad 0x00af9a000000ffff // 0x08: 64-bit code, as in the kernel GDT
    .quad 0x00cf92000000ffff // 0x10: data
    .quad 0x00cf9a000000ffff // 0x18: 32-bit code
trampoline_gdtr:
    .word trampoline_gdtr - trampoline_gdt - 1
    .long ABS(trampoline_gdt)

// Parameter block, written by smp.cpp (TrampolineParameters).
.align 8
.global ok_x86_smp_trampoline_parameters
ok_x86_smp_trampoline_parameters:
trampoline_arrived:
    .quad 0
trampoline_cr0:
    .quad 0
trampoline_cr3:
    .quad 0
trampoline_cr4:
    .quad 0
trampoline_entry:
    .quad 0
trampoline_apic_id: // initial APIC id + 1; zero matches no CPU
    .quad 0
trampoline_stack:
    .quad 0
trampoline_cpu:
    .quad 0
.global ok_x86_smp_trampoline_end
ok_x86_smp_trampoline_end:

.section .note.GNU-stack,"",@progbits
