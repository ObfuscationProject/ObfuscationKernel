.section .rodata.probe,"a"
.balign 4096
.global ok_la_probe_begin, ok_la_probe_timer, ok_la_probe_ro, ok_la_probe_kernel, ok_la_probe_end
ok_la_probe_begin:
    li.d $a0, 42
    li.d $a7, 65
    syscall 0
    li.d $a7, 66
    syscall 0
    b .
ok_la_probe_timer:
    addi.d $s0, $s0, 1
    b ok_la_probe_timer
ok_la_probe_ro:
    li.d $t0, 0x40000000
    st.d $zero, $t0, 0
    syscall 0
ok_la_probe_kernel:
    li.d $t0, 0x90000000
    ld.d $a0, $t0, 0
    syscall 0
.global ok_la_probe_read, ok_la_probe_fp, ok_la_probe_invalid, ok_la_probe_nx
ok_la_probe_read:
    ld.d $a0, $sp, 0
    li.d $a7, 99
    syscall 0
ok_la_probe_fp:
    li.d $t0, 0x123456789abcdef
    movgr2fr.d $f24, $t0
    li.d $t0, 1
    movgr2cf $fcc3, $t0
    li.d $tp, 0x778899
    li.d $a7, 77
    syscall 0
    movfr2gr.d $a0, $f24
    movcf2gr $a1, $fcc3
    move $a2, $tp
    li.d $a7, 78
    syscall 0
ok_la_probe_invalid:
    .word 0
ok_la_probe_nx:
    li.d $t0, 0x40001000
    jirl $zero, $t0, 0
ok_la_probe_end:
