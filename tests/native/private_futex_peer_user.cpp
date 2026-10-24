// Raw Linux-personality ELF; the debug harness supplies a role on each stack.
#include "private_futex_peer_shared.hpp"
using word = __UINTPTR_TYPE__;
using signed_word = __INTPTR_TYPE__;
extern "C" signed_word ok_peer_syscall4(word, word, word, word, word);
extern "C" { [[gnu::section(".peer_state")]] ok::tests::PeerState ok_peer_shared{}; }
#if defined(OK_ARCH_TARGET_X86_64)
constexpr word futex_number = 202;
#elif defined(OK_ARCH_TARGET_I386) || defined(OK_ARCH_TARGET_ARM32)
constexpr word futex_number = 240;
#elif defined(OK_ARCH_TARGET_RV32)
constexpr word futex_number = 422;
#elif defined(OK_ARCH_TARGET_MIPS)
constexpr word futex_number = 4238;
#elif defined(OK_ARCH_TARGET_MIPS64)
constexpr word futex_number = 5194;
#elif defined(OK_ARCH_TARGET_PPC)
constexpr word futex_number = 221;
#else
constexpr word futex_number = 98;
#endif
extern "C" [[noreturn]] void ok_peer_main(word role)
{
    auto &s = ok_peer_shared;
    if (role < 4)
        for (unsigned round = 0; round < 2; ++round)
        {
            __atomic_store_n(&s.entered[role], round + 1, __ATOMIC_RELEASE);
            if (role < 3)
            {
                const auto result = ok_peer_syscall4(futex_number, reinterpret_cast<word>(&s.word), 128, 7, 0);
                s.results[round][role] = static_cast<ok::tests::PeerWord>(result);
                s.observed[round][role] = __atomic_load_n(&s.word, __ATOMIC_ACQUIRE);
            }
            else
            {
                __atomic_store_n(&s.word, 99, __ATOMIC_RELEASE);
                constexpr word counts[] = {0, 1, 0xffffffffU};
                for (unsigned i = 0; i < 3; ++i)
                    s.wakes[round][i] = static_cast<ok::tests::PeerWord>(
                        ok_peer_syscall4(futex_number, reinterpret_cast<word>(&s.word), 129, counts[i], 0));
            }
            __atomic_store_n(&s.completed[role], round + 1, __ATOMIC_RELEASE);
            const auto parked = ok_peer_syscall4(futex_number, reinterpret_cast<word>(&s.park), 128, 0, 0);
            if (parked != 0)
                s.failure[role] = static_cast<ok::tests::PeerWord>(parked);
        }
    for (;;) asm volatile("" ::: "memory");
}
