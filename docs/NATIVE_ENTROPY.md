# Trusted entropy and Linux initial-stack randomness

SDK35 introduces a runtime EntropySource device/service boundary. The
distribution installs a boot-lifetime provider; the kernel does not infer
entropy from clocks, scheduling ticks or device presence. Nonempty getrandom
without a provider returns EAGAIN. Zero bytes succeed and unknown flags fail
with EINVAL. Only flags zero and GRND_NONBLOCK are implemented in this bounded
slice; Linux indefinite blocking, GRND_RANDOM, /dev/random and kernel CSPRNG
reseed semantics remain unimplemented.

The source must fill its entire private staging span on success. The kernel
publishes completed chunks of at most 256 bytes. Failure in the first chunk
leaves output unchanged; failure later returns the committed prefix, with the
uncommitted suffix unchanged. Linux exec requests sixteen bytes before
publishing a replacement image, stores them on its user stack and exports
AT_RANDOM. Entropy failure discards the unpublished image and preserves the
original native process. Native-personality exec does not require AT_RANDOM.

ObfuscationOS supplies modern Virtio RNG transports for the ten QEMU reference
profiles. Their explicit rng-random backend trusts host /dev/urandom and the
hypervisor. Passing byte-transfer tests does not measure entropy quality or
establish physical hardware support. These transports poll on a serialized
boot CPU with static private DMA backing. A failed queue remains retired for
the boot; its backing outlives possible late DMA. IRQ, SMP, hot-unplug, platform
resource arbitration and physical DMA/cache-coherency qualification remain
separate gates.

The public C++ service layout changes require both SDK35 and native module
ABI35. OS builds consume explicit matching exported artifacts. Hosted entropy
and initial-stack tests are models; actual guest probes, no-device rollback,
standalone kernel boots and final offline filesystem checks have separate
evidence scopes. Offline e2fsck is not a synchronized shutdown or durability
qualification.
