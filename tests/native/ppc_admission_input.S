.section .rodata
.balign 16
.global ok_ppc_admission_elf_begin,ok_ppc_admission_elf_end
ok_ppc_admission_elf_begin:
.incbin "/media/data1/Project/System/ObfuscationProject/build/prototypes/native-signal-ppc-readiness-v2/inputs/linux.elf"
ok_ppc_admission_elf_end:
.section .note.GNU-stack,"",@progbits
