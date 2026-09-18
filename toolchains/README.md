# Toolchains

ObfuscationKernel no longer installs compilers or QEMU in this directory.
Freestanding GCC/binutils and the matching QEMU emulator come from the
`systoolchain` xmake package and are stored in xmake's package cache.

Run `xmake f -c -y -a <arch>` to build and resolve SysToolchain from source.
Package installation performs the compiler, binary-utility, and QEMU checks.

To reuse an existing installation without package downloads, configure
`--systoolchain_dir=/absolute/path/to/installation`. That directory must contain
`bin/<triple>-gcc`, G++, Binutils and, for QEMU tasks, the matching emulator.
No toolchain is copied into this repository. A multi-architecture matrix needs a
prefix with every selected compiler, or individually configured architecture runs.
