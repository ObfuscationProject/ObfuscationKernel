# Static syscall restart preparation

`RestartAdapter<Profile>` prepares a scalar Linux read frame from an original
syscall-input snapshot. All ten architecture templates are retained. It checks
the returned backend trap classification, instruction range/alignment, target
word width and exact instruction class. The callback form requests the complete
instruction through checked executable user access. Frame preparation preserves
all original registers, FP/SPE state, TLS and flags, changing the restart PC,
ARM's PC mirror and PPC's Linux-visible CR0.SO only. PPC result metadata is zero.

This helper is pure preparation, not runtime restart support. Neither its public
Plan nor a supplied Trap proves that an owned backend capture happened. Runtime
integration must bind the real entry-stack lease, image, address space, capture
sequence and wait ticket before storing a plan. An EINTR/result-bearing or
sigreturn frame is not an original capture. n64/PPC readiness must publish a
separate typed transaction; ordinary result completion cannot authorize it.
Handler-edited return contexts must remain authoritative without hidden replay.

The AArch64 assembly's raw synchronous vector is 8, but the returned C++ Trap
stores ESR.EC (0x15) in vector and the syndrome in error_code. The helper validates
the returned representation. RISC-V always rewinds four bytes while allowing
two-byte instruction alignment. ARM derives Thumb width from captured CPSR.T,
preserving IT/condition bits. This does not establish legal IT guest execution.
MIPS branch-delay captures are rejected because normalized continuation has
already lost raw EPC. Ordinary branch-delay syscall completion is unchanged.

The hosted ASan/UBSan test exercises all ten templates and both ARM widths with
literal target opcode bytes and independent expected full frames. It covers
range edges, crossing-page copy rejection, target-word overflow, nonboolean MIPS
a3, PPC SO initially clear/set, wrong trap kinds, wrong number/encoding, invalid
copy and the MIPS BD gate. These are host tests, not target instruction execution,
owned-capture lifecycle tests, pipe interruption, signal return, SMP or physical
qualification. No runtime call site or exported ABI layout is changed here.
