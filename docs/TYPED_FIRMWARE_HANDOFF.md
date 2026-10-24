# Typed firmware memory discovery

`ok/boot/firmware_handoff.hpp` connects the owned raw boot capture to bounded
E820/FDT decoders. It preserves complete parser records, including 64-bit
physical ranges on 32-bit profiles. It does not create allocator input, install
page mappings, reserve RAM, or replace the existing bootstrap pool.

The caller supplies a known `BootProtocol`, a statically dispatched
`FirmwareReader`, a caller-owned `FirmwareMemory` workspace and a separate
output. Architecture selection is a template argument; no virtual service is
used. Unknown protocols and absent raw sources fail before the reader is called.
No failed discovery manufactures a memory map.

The reader owns physical-to-virtual translation and mapping authority. It must
return an exactly sized, stable, already mapped readable span or an error. A
pointer cast without established mapping bounds does not satisfy this contract.
The header is read first, checked, and copied into owned temporary storage;
then the bounded full source is requested. The decoder checks the complete
header against the first copy. Firmware must remain immutable throughout the
call. Reader spans must remain valid until discovery returns.

On success the output owns both raw provenance and format-specific parser
records, with no retained pointers into firmware. `source_storage` names the
full firmware allocation that later reservation policy must exclude;
`decoded_bytes` names the bytes read. BIOS reserves the complete fixed
`[0x5000,0x5c10)` table even when fewer than 128 records were returned. FDT storage
uses its checked total size. Failure preserves the output; workspace contents
are unspecified. Workspace/output aliasing is rejected. Diagnostic E820 partial
count is returned separately on incomplete/full source failures.

| Profile | Explicit protocol and current decision |
| --- | --- |
| i386, x86_64 | `bios_e820`: require the collector tag, exact `0x5000` table address, complete header, and bounded records. |
| aarch64 | `aarch64_linux_image`: use aligned x0 DTB, require reserved x1/x2/x3 to be zero. |
| rv32, rv64 | `riscv_fdt`: use aligned a1 DTB; retain hart ID and other raw words as provenance. |
| ppc | `epapr`: require ePAPR magic, use aligned r3 DTB, and enforce the r7 initial mapping limit on both reads. |
| arm32 | `arm32_linux_image`: require r0=0, r1=0xffffffff and aligned representable r2 FDT; preserve unspecified r3. Historical `arm32_direct_elf` zero tuple remains `source_absent`. |
| loongarch64 | `loongarch64_direct_elf`: current zero tuple reports `source_absent`; no guessed source or masked high address cells. |
| mips, mips64 | `malta_prom`: explicitly unsupported until bounded 32-bit PROM table/string decoding is qualified; a3 alone is not a complete map. |

A parsed map remains unqualified for allocation. Platform checks must validate
RAM against the selected machine, translate every loaded-object reservation,
reserve the firmware storage, kernel/NOLOAD regions, stacks, page tables, boot
payloads and archives, subtract all exclusions, and apply explicit mapping,
capacity, accounting and lifetime policies. A 64-bit source range is not proof
that the current native backend can address it.

## Validation

Hosted checks exercise source selection for all ten profiles, full-width source
ownership, unknown E820 attributes, FDT reservations, incomplete/capacity/parser
failures, bad reader spans, changed headers, output preservation and ePAPR
bounds. They do not execute target code:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 scripts/test_firmware_handoff.py --sanitize
python3 tests/host/firmware_handoff_acceptance_tests.py
```

The separate native runner directly compiles the actual boot/capture/FDT sources
and typed discovery probe for each requested target. It validates explicit
compiler identity, keeps RTTI, and does not alter xmake configuration or depend
on ObfuscationOS. Its target flags are recorded in
`scripts/native_firmware_profiles.json`. Results snapshot exact source inputs,
commands, compiler/QEMU hashes, linked artifacts and serial transcripts in a
new output directory; existing results are never overwritten.

```sh
python3 scripts/test_native_firmware_handoff.py --output build/firmware-native
python3 scripts/test_native_firmware_handoff.py --output build/firmware-high \
  --arch i386 --arch x86_64 --x86-high
python3 scripts/test_native_firmware_handoff.py --output build/firmware-el2 \
  --arch aarch64 --aarch64-el2
python3 scripts/test_native_firmware_handoff.py --output build/firmware-short \
  --arch i386 --arch x86_64 --arch aarch64 --arch arm32 --arch rv64 --arch rv32 --arch ppc \
  --header-only
```

The probe reader uses explicit mappings established by this QEMU launch and
bootstrap code. These test-specific apertures are not a production firmware
mapper or a general platform validator. The normal runs execute actual captured
arguments through bounded reads and decode genuine QEMU firmware. Short-window
runs execute synthetic mapping rejection in QEMU. Serial fixture tests exercise
only the host acceptance parser. No result qualifies physical firmware, enables
firmware-backed native allocation, or establishes SMP ownership.

ARM entry state (CPSR/SCTLR/MPIDR) is captured by assembly and checked before
firmware dereference. `--arch arm32 --arm32-direct-elf` launches the new diagnostic
ELF and requires protocol rejection with zero reads. This differs from the
hosted selector test and historical fixture for explicit `arm32_direct_elf`.
