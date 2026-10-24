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
.global ok_audit_capture, ok_audit_loop, ok_audit_usprg
ok_audit_capture:
    sc
    sc
    sc
ok_audit_loop:
    b ok_audit_loop
ok_audit_usprg:
    mfspr 10,256
    mtspr 256,9
    sc
    b .
.global ok_audit_save
ok_audit_save:
    // Independent user writes exercise capture as well as restoration.
    lis 0,0xa0b1
    ori 0,0,0xc2d4
    evmergelo 0,0,0
    lis 0,0x4c35
    ori 0,0,0x8617
    evmra 0,0
    lis 0,0x0000
    ori 0,0,0x0002
    mtspr 512,0
    lis 0,0x7974
    ori 0,0,0xd678
    mtlr 0
    lis 0,0xbdb9
    ori 0,0,0x1abc
    mtctr 0
    lis 0,0xb468
    ori 0,0,0xace1
    mtcr 0
    lis 0,0xe000
    ori 0,0,0x0045
    mtxer 0
    lis 0,0xdf96
    ori 0,0,0xb412
    mtspr 256,0
    lis 0,0xe742
    ori 0,0,0x8003
    evmergelo 1,0,1
    lis 0,0xe744
    ori 0,0,0x8006
    evmergelo 2,0,2
    lis 0,0xe746
    ori 0,0,0x8009
    evmergelo 3,0,3
    lis 0,0xe748
    ori 0,0,0x800c
    evmergelo 4,0,4
    lis 0,0xe74a
    ori 0,0,0x800f
    evmergelo 5,0,5
    lis 0,0xe74c
    ori 0,0,0x8012
    evmergelo 6,0,6
    lis 0,0xe74e
    ori 0,0,0x8015
    evmergelo 7,0,7
    lis 0,0xe750
    ori 0,0,0x8018
    evmergelo 8,0,8
    lis 0,0xe752
    ori 0,0,0x801b
    evmergelo 9,0,9
    lis 0,0xe754
    ori 0,0,0x801e
    evmergelo 10,0,10
    lis 0,0xe756
    ori 0,0,0x8021
    evmergelo 11,0,11
    lis 0,0xe758
    ori 0,0,0x8024
    evmergelo 12,0,12
    lis 0,0xe75a
    ori 0,0,0x8027
    evmergelo 13,0,13
    lis 0,0xe75c
    ori 0,0,0x802a
    evmergelo 14,0,14
    lis 0,0xe75e
    ori 0,0,0x802d
    evmergelo 15,0,15
    lis 0,0xe760
    ori 0,0,0x8030
    evmergelo 16,0,16
    lis 0,0xe762
    ori 0,0,0x8033
    evmergelo 17,0,17
    lis 0,0xe764
    ori 0,0,0x8036
    evmergelo 18,0,18
    lis 0,0xe766
    ori 0,0,0x8039
    evmergelo 19,0,19
    lis 0,0xe768
    ori 0,0,0x803c
    evmergelo 20,0,20
    lis 0,0xe76a
    ori 0,0,0x803f
    evmergelo 21,0,21
    lis 0,0xe76c
    ori 0,0,0x8042
    evmergelo 22,0,22
    lis 0,0xe76e
    ori 0,0,0x8045
    evmergelo 23,0,23
    lis 0,0xe770
    ori 0,0,0x8048
    evmergelo 24,0,24
    lis 0,0xe772
    ori 0,0,0x804b
    evmergelo 25,0,25
    lis 0,0xe774
    ori 0,0,0x804e
    evmergelo 26,0,26
    lis 0,0xe776
    ori 0,0,0x8051
    evmergelo 27,0,27
    lis 0,0xe778
    ori 0,0,0x8054
    evmergelo 28,0,28
    lis 0,0xe77a
    ori 0,0,0x8057
    evmergelo 29,0,29
    lis 0,0xe77c
    ori 0,0,0x805a
    evmergelo 30,0,30
    lis 0,0xe77e
    ori 0,0,0x805d
    evmergelo 31,0,31
    lis 0,0xe740
    ori 0,0,0x8000
    evmergelo 0,0,0
    lis 1,0x8000
    ori 1,1,0x0000
    lis 2,0x4000
    ori 2,2,0x2040
    lis 3,0x6743
    ori 3,3,0x8003
    lis 4,0x6744
    ori 4,4,0x8004
    lis 5,0x6745
    ori 5,5,0x8005
    lis 6,0x6746
    ori 6,6,0x8006
    lis 7,0x6747
    ori 7,7,0x8007
    lis 8,0x6748
    ori 8,8,0x8008
    lis 9,0x6749
    ori 9,9,0x8009
    lis 10,0x674a
    ori 10,10,0x800a
    lis 11,0x674b
    ori 11,11,0x800b
    lis 12,0x674c
    ori 12,12,0x800c
    lis 13,0x4000
    ori 13,13,0x2080
    lis 14,0x674e
    ori 14,14,0x800e
    lis 15,0x674f
    ori 15,15,0x800f
    lis 16,0x6750
    ori 16,16,0x8010
    lis 17,0x6751
    ori 17,17,0x8011
    lis 18,0x6752
    ori 18,18,0x8012
    lis 19,0x6753
    ori 19,19,0x8013
    lis 20,0x6754
    ori 20,20,0x8014
    lis 21,0x6755
    ori 21,21,0x8015
    lis 22,0x6756
    ori 22,22,0x8016
    lis 23,0x6757
    ori 23,23,0x8017
    lis 24,0x6758
    ori 24,24,0x8018
    lis 25,0x6759
    ori 25,25,0x8019
    lis 26,0x675a
    ori 26,26,0x801a
    lis 27,0x675b
    ori 27,27,0x801b
    lis 28,0x675c
    ori 28,28,0x801c
    lis 29,0x675d
    ori 29,29,0x801d
    lis 30,0x675e
    ori 30,30,0x801e
    lis 31,0x675f
    ori 31,31,0x801f
    lis 0,0x6740
    ori 0,0,0x8000
    sc
    sc
    b .
.global ok_ppc_program_illegal,ok_ppc_program_privileged,ok_ppc_program_trap
ok_ppc_program_illegal:
    .long 0x7fffffff
    b .
ok_ppc_program_privileged:
    mfmsr 3
    b .
ok_ppc_program_trap:
    tw 31,0,0
    b .
ok_native_user_end:
.section .note.GNU-stack,"",@progbits
