# Linux pathname bootstrap calls

The selected Linux personality now admits getcwd, chdir, fchdir, readlink and
readlinkat. Common POSIX/VFS operations retain ownership, search permissions,
cwd/dirfd identity and checked metadata errors. The Linux adapter supplies the
Linux-only scalar and return conventions; the existing native personality is
unchanged. No new runtime virtual interface or syscall number was invented.

Raw Linux getcwd returns the number of bytes including the terminating NUL.
Capacity smaller than that count returns ERANGE. Only the actual returned range
must be writable; unused advertised capacity is not touched. Renamed cwd objects
retain identity through the common resolver. A missing/unlinked cwd keeps its
common failure rather than returning an invented pathname.

Readlink/readlinkat use Linux's signed 32-bit syscall buffer length, including on
LP64; the musl public API's size_t parameter does not change that kernel ABI.
Nonpositive syscall lengths fail with EINVAL. Copies contain exactly the link
bytes, without a terminating NUL; a shorter positive buffer truncates them.
The common native path validates only returned bytes, so a successful short
read can end at a user mapping boundary. O_PATH symlink descriptors and Linux's
empty-path readlinkat extension remain unsupported. The existing 255-byte path,
31-byte component and bounded link payload limits remain explicit.

The generated syscall tables pin Linux 6.13.4 and musl 1.2.5 source inputs.
scripts/test_linux_paths.py --sanitize exercises all ten ABI adapters over the
actual native/common call path with hosted mappings; it is not guest execution.
The OS paths.c probe preserves startup/file/heap/process acceptance and adds
actual musl cwd/readlink calls, Linux sizes, failures and a PROT_NONE mapping-edge
copy test. Its --require-paths receipt requires two complete newline-terminated
path markers and every preceding process/file/heap gate. Kernel and OS inputs
remain frozen throughout each explicit standalone/SDK-consumer native matrix.
