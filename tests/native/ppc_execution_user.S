.machine e500
.section .rodata.native_user,"a"
.global ok_native_user_begin, ok_native_user_end
.global ok_native_user_kernel_read, ok_native_user_readonly_write
.global ok_native_user_nx, ok_native_user_unmapped, ok_native_user_invalid
ok_native_user_begin:
    lis 14,0x1122
    ori 14,14,0x3344
    lis 15,0x55aa
    ori 15,15,0xaa55
    evmergelo 14,15,14
    evmra 14,14
    lis 31,0x8877
    ori 31,31,0x6655
    li 15,2
    mtspr 512,15
    lis 15,0x2468
    ori 15,15,0xace0
    mtcr 15
    lis 15,0x1234
    ori 15,15,0x5678
    mtctr 15
    li 15,0x4567
    stw 15,0(2)
    li 15,0x7654
    stw 15,0(13)
    li 15,0x789a
    mtspr 256,15
    li 3,0x23
    li 4,42
    li 0,0x1234
    sc
    lis 5,0x4000
    ori 5,5,0x2000
    stw 3,0(5)
    mfspr 15,256
    stw 15,40(5)
    evstdd 14,16(5)
    evxor 15,15,15
    evmwumiaa 15,15,15
    evstdd 15,24(5)
    li 0,0x2345
    sc
1:  b 1b
ok_native_user_kernel_read:
    lis 3,0x0100
    lwz 4,0(3)
    .long 0
ok_native_user_readonly_write:
    lis 3,0x4000
    stw 4,0(3)
    .long 0
ok_native_user_nx:
    lis 3,0x4000
    ori 3,3,0x2000
    lwz 4,0(3) // Fill the NX data TLB before the instruction fetch.
    mtctr 3
    bctr
ok_native_user_unmapped:
    lis 3,0x6000
    lwz 4,0(3)
    .long 0
ok_native_user_invalid:
    mfmsr 3
.global ok_native_user_tlb_stress
ok_native_user_tlb_stress:
    lis 3,0x4100
    li 4,20
    li 5,1
1:  stw 5,0(3)
    addi 3,3,4096
    addi 5,5,1
    addic. 4,4,-1
    bne 1b
    lis 3,0x4100
    lwz 6,0(3)
    li 0,0x6789
    sc
    b .
ok_native_user_end:
.section .note.GNU-stack,"",@progbits
