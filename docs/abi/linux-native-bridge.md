# Saved native frame to Linux ABI bridge

Status: source-audited bridge helpers, hosted models and isolated freestanding
compilation for all ten profiles. This is **not native Linux syscall or musl
execution qualification**. No existing runtime, architecture, dispatcher,
process, xmake or ledger file is changed by this bridge batch.

`include/ok/abi/linux_native.hpp` supplies `linux_user::NativeBridge<Profile>`
and the compile-time `ConfiguredNativeBridge` alias. It translates the existing
`arch::execution::UserFrame` representation into the independently generated
`Codec<Profile>` register representation. It never reinterprets one structure
as the other. Thirty-two-bit register words are canonicalized to their low
32 bits, independent of the host compiler's pointer width or byte order.

## API and ownership

- `snapshot(frame)` copies saved integer state into `RegisterFile`. It accounts
  for architectural stack pointers stored separately, ignores private non-GPR
  slots, and extracts PowerPC CR from its actual saved byte location.
- `decode_call(frame, count, copied_stack_from_sp={})` returns a raw `Call` with
  at most six arguments. The caller supplies the signature-specific slot count.
  MIPS O32 uses copied target-endian words at SP+16 and SP+20 for slots five/six.
  A count of four requires no user-stack bytes.
- `decode_call_copying_stack(frame, count, callback)` requests only the O32 stack
  words actually needed. The callback has signature
  `Status(uptr source, std::span<u8> destination)` and owns user-range checking,
  complete copy and process/mapping lifetime. Address arithmetic rejects target
  word wrap before the callback runs. No other profile invokes the callback.
  Copy failures publish no call and do not change the frame.
- `encode_return(frame, Return)` and `decode_return(frame)` delegate error/value
  validation to the codec. Encoding changes only the actual result register,
  MIPS a3 when applicable, and PowerPC CR0.SO when applicable. All unrelated
  register, control, SIMD/FP and TLS state is preserved. Invalid encodings leave
  the entire frame unchanged.
- `set_thread_pointer(frame, ThreadPointer{value}, route)` updates the state that
  the existing entry/return path actually consumes. It checks target word width;
  the caller must separately enforce its user-address and native-entry policy.
  The default route is `restored_register`. MIPS requires an explicit
  `mips_rdhwr_emulation` route; requesting hardware restoration there returns
  unsupported without modifying the frame. A route inappropriate to any other
  profile also returns unsupported.

No helper infers pointer/scalar signedness, padding, reordered syscall arguments,
`clone` signatures, or mmap byte/page offsets. The caller explicitly selects
`Codec::decode_argument64(call, first_slot)` when a signature needs a word pair.
MIPS O32 indirect syscall multiplexing and calls with seven/eight raw arguments
remain outside this batch. A 64-bit syscall number above the codec's canonical
u32 number domain is rejected; Linux entry-specific number truncation, unknown
call handling, restart and tracing behavior remain dispatcher work.

## Audited frame and trap paths

The source audit covers each `src/arch/<profile>/execution.cpp` and
`execution.S`, plus the shared MIPS implementation. Musl register/TLS conventions
come from the pinned official musl 1.2.5 archive, including each
`arch/<port>/syscall_arch.h`, `pthread_arch.h` and available architecture
`__set_thread_area` implementation. The existing
[codec manifest](linux-profiles.json) records the archive and syscall-file hashes.

| Profile | Number; raw argument locations in native frame | Return encoding | Restored Linux thread pointer |
| --- | --- | --- | --- |
| i386 | slot 7; slots 4,6,5,1,0,2 | slot 7, negative errno | `gs_base`, GDT entry 7; GS selector slot 8 becomes `0x3b` |
| x86_64 | slot 14; slots 9,10,11,5,7,6 | slot 14, negative errno | `fs_base` through IA32_FS_BASE before user entry |
| aarch64 | x8; x0–x5 | x0, negative errno | `fs_base` through TPIDR_EL0 |
| arm32 | r7; r0–r5 | r0, negative errno | **`gs_base`** through TPIDRURO, CP15 c13,c0,3 |
| rv64 | x17/a7; x10–x15 | x10/a0, negative errno | saved/restored x4/tp in `registers[4]` |
| rv32 | x17/a7; x10–x15 | x10/a0, negative errno | saved/restored x4/tp in `registers[4]` |
| loongarch64 | r11/a7; r4–r9 | r4/a0, negative errno | r2/tp in `registers[2]`; `fs_base` remains its capture mirror |
| mips | r2/v0; r4–r7, copied SP+16/+20 | r2 plus r7/a3=1 for positive errno | Explicit real RI-trap RDHWR `$29` emulation from `fs_base`; no UserLocal register restore |
| mips64 | r2/v0; r4–r9 | r2 plus r7/a3=1 for positive errno | Same explicit emulation route |
| ppc | r0; r3–r8 | r3 plus CR0.SO bit 28 for positive errno | r2 from `fs_base`; update both r2 and its entry mirror |

The x86 private arrays reflect their assembly push order, not DWARF ordering.
i386 slot 3 is the `pushal` stack value, not architectural user ESP; user ESP is
`frame.sp`. Slots 8–12 hold selectors. On x86_64 the first saved slot is R15,
and user RSP is separate. On AArch64 slot 31 is packed FPCR/FPSR and must not be
copied as XZR or SP. MIPS K0/K1 are trap scratch and are not saved user GPRs.

PowerPC `frame.flags` is saved MSR. Its **CR is the big-endian word at
`fp_state[280..283]`**, absolute frame offset 440 on the 32-bit profile. Entry
loads it with `lwz`/`mtcr`; trap capture stores it at the same offset. The bridge
must preserve MSR, LR, CTR, XER, SPE upper halves, ACC and SPEFSCR. PowerPC entry
also copies `fs_base` to r2 and `gs_base` to r13; Linux uses r2 for TLS, so the
helper preserves the unrelated r13/GS state.

ARM32 restores both CP15 thread ID registers, but musl's ARMv7 `__get_tp` reads
the user-read-only register c13,c0,3. Merely changing `fs_base` would update the
wrong register. Older ARM kuser helper addresses/HWCAP startup are not supplied
by this bridge. i386's fixed GDT slot is an encoding mechanism; decoding Linux
`user_desc`, validating its flags/index and copying entry number 7 back to a
requesting libc remain explicit personality-handler work. AArch64/RISC-V/
LoongArch/PowerPC libc may update its TP directly in user mode; their existing
trap capture/return path preserves that register, independently of this setter.

MIPS Malta backends do not restore CP0.UserLocal. Their `enter()` recognizes a
real reserved-instruction exception for RDHWR register 29, writes `fs_base` into
the requested saved GPR and computes the continuation before re-entering user
mode. The opt-in setter targets that existing path only; it does not advertise
hardware UserLocal, arbitrary MIPS TLS instruction forms, or a working Linux
`set_thread_area` dispatcher.

## Validation and remaining gates

```sh
python3 scripts/test_linux_native_bridge.py
python3 scripts/test_linux_native_bridge.py --sanitize
python3 scripts/test_linux_native_bridge.py --cross \
  --toolchain-root=/explicit/compiler/root \
  --musl-archive=/explicit/path/musl-1.2.5.tar.gz
```

The normal runner checks critical save/restore source anchors and exercises all
ten bridge specializations on the host. Literal native-slot vectors verify
32/64-bit arguments, O32 big-endian stack words, exact copy lengths and wrap
faults, negative errno, MIPS a3, PowerPC CR bytes, TLS route rejection and
unrelated-state preservation. These are host models and source audits.

`--cross` compiles only `tests/host/linux_native_bridge_compile.cpp` with ten
explicit compiler paths into temporary objects. Static assertions verify target
word width and actual `UserFrame` offsets; ELF checks verify class, byte order
and machine. It neither configures xmake nor links or executes a kernel. The
optional archive check verifies the exact musl pin and all ten syscall header
identities without extraction or downloads.

The Linux ELF/personality selection, trap dispatch, user-copy integration,
syscall-specific layouts, full errno/flags, startup/auxv, TLS allocation,
pthread/signal semantics and musl guest acceptance are still separate gates.
This bridge must enter native qualification through a Linux personality lane
that also preserves existing native-personality acceptance and exported SDK
consumer validation. General SMP safety is not established by these helpers.
