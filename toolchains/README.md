# Toolchains

ObfuscationKernel no longer installs compilers or QEMU in this directory.
Freestanding GCC/binutils and the matching QEMU emulator come from the
`systoolchain` xmake package and are stored in xmake's package cache.

Run `xmake f -c -y -a <arch>` to build and resolve SysToolchain from source.
Package installation performs the compiler, binary-utility, and QEMU checks.
