# Native user runtime layout

Target of `refactor/user-runtime-split`: `runtime.cpp` (5,876 lines, 101 includes
at the start of the worktree) splits into units with one responsibility each.
`scripts/check_runtime_split.py` is a ratchet: it fails when `runtime.cpp` grows
past the recorded size or include count, and the recorded numbers are lowered as
code moves out.

| Unit | Responsibility | Moves from |
| --- | --- | --- |
| `loader.cpp` | ELF admission, argv/envp/auxv, stack and address-space setup | `runtime.cpp` |
| `entry.cpp` | Native loop, `enter_owned`, syscall/timer/fault dispatch, entry-stack leases | `runtime.cpp` |
| `vm.cpp` | mmap/mprotect/munmap/brk, page pools, shared files | `runtime.cpp`, `native_*page*.hpp` |
| `wait.cpp` | wait4, poll/select, sleep, futex, child wait | `runtime.cpp`, `native_child_wait_runtime.hpp`, `native_poll_mask.hpp` |
| `pty.cpp` | PTY pair runtime and owners | `native_pty_*.hpp` |
| `net.cpp` | UDP/TCP runtime adapters | `linux_udp_*.hpp`, `native_udp_*.hpp`, `tcp_*.hpp` |
| `signals/` | Delivery, frames, return; one `include/ok/arch/<arch>/signal.hpp` per architecture replaces the 30 per-architecture headers | `signals/*.hpp`, `include/ok/user/native_signal_*_<arch>.hpp` |

Rules: no behaviour change in this worktree; each extraction is one commit that
moves code and its tests and is validated by `xmake test` on the fast lane
(x86_64, arm32, ppc) plus the hosted runtime suites. The release image must not
grow.
