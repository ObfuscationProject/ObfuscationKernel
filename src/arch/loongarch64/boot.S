.section .text.boot, "ax"
.global _start
.type _start, @function
_start:
    la.local $sp, boot_stack_top
    la.local $t0, _bss_start
    la.local $t1, _bss_end
1:
    bgeu $t0, $t1, 2f
    st.d $zero, $t0, 0
    addi.d $t0, $t0, 8
    b 1b
2:
    bl kernel_main
3:
    idle 0
    b 3b

.section .bootstrap_bss, "aw", @nobits
.align 12
boot_stack:
    .skip 1048576
boot_stack_top:
