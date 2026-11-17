#pragma once

#include "ok/arch/execution.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_section.hpp"
#include "ok/smp/lock.hpp"

#include <atomic>
#include <type_traits>
#include <utility>

// User access for kernel code: the backend copies (ok/arch/execution.hpp) plus
// the recovery they leave to the runtime. A backend walks the translation
// tables inside a kernel window and refuses a page whose write access is
// withheld (copy-on-write), or a translation in the middle of a replacement.
// A failed access here:
//   1. has closed its window (the backend call returned);
//   2. takes mm of the process that owns the space (the caller's own; the
//      native runtime asserts that) and resolves every withheld page of the
//      range, then releases mm;
//   3. retries, up to max_retries times while something was resolved;
//   4. makes a final attempt holding mm, which no translation writer can then
//      change: the result is success or a genuine fault.
// Inside a kernel section nothing is resolved (A6) and the first result
// stands. Model builds without the native runtime resolve nothing.
//
// No user access runs under a class A lock (I2, counted as class_asserts),
// except with_user_word: one word, no fault, inside its own kernel window.
namespace ok::user::uaccess
{
namespace hw = arch::execution;

inline constexpr usize max_retries = 4;

// One access attempt, called with or without mm held.
class Attempt final
{
  public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, Attempt>)
    explicit Attempt(F &function) noexcept
        : context_(&function), call_([](void *context) { return (*static_cast<F *>(context))(); })
    {
    }
    [[nodiscard]] Status operator()() const
    {
        return call_(context_);
    }

  private:
    void *context_;
    Status (*call_)(void *);
};

} // namespace ok::user::uaccess

namespace ok::user::native
{
// The native runtime's resolution (src/user/runtime/vm.cpp), linked only
// where the runtime is. Takes mm of the process whose live space is `space`
// (re-entrant on the CPU that holds it) and makes every withheld page of
// [address, address + size) writable for `write`. Without `attempt` it
// returns success when it resolved a page, not_found when none was withheld,
// no_process when no live process owns the space and an error otherwise,
// releasing mm; with `attempt` it runs the attempt before releasing mm and
// returns its status.
Status resolve_user_access(const arch::execution::AddressSpace &space, uptr address, usize size, bool write,
                           const uaccess::Attempt *attempt) __attribute__((weak));
} // namespace ok::user::native

namespace ok::user::uaccess
{
namespace detail
{
inline void count(std::atomic<uptr> smp::EvidenceCpu::*counter) noexcept
{
    if (const auto cpu = smp::current_cpu(); cpu < smp::max_cpus)
        smp::detail::owner_add(smp::evidence_cpus[cpu].*counter);
}

// Steps 2-4 above for an access whose first attempt returned `failed`.
inline Status recover(const hw::AddressSpace &space, uptr address, usize size, bool write, const Attempt &attempt,
                      Status failed)
{
    if (&native::resolve_user_access == nullptr)
        return failed;
    if (smp::in_section())
    {
        // A6: a section never rewrites page records; admission resolved its
        // output range under the kernel lock. A write that still meets a
        // readable but unwritable page would need a resolution (counted); any
        // other failure is the caller's fault.
        if (write && hw::user_range(space, address, size, false))
            smp::detail::section_assert(&smp::SectionCpu::cow_refused, "copy-on-write resolution inside a section");
        return failed;
    }
    for (usize retry = 0; retry < max_retries; ++retry)
    {
        const auto resolved = native::resolve_user_access(space, address, size, write, nullptr);
        if (resolved.code() == StatusCode::not_found)
            break;
        // No live process owns the space: nothing to resolve or hold.
        if (resolved.code() == StatusCode::no_process || !resolved)
            return failed;
        count(&smp::EvidenceCpu::uaccess_retries);
        if (auto status = attempt(); status)
            return status;
    }
    count(&smp::EvidenceCpu::uaccess_held);
    return native::resolve_user_access(space, address, size, write, &attempt);
}
} // namespace detail

[[nodiscard]] inline Status copy_from_user(const hw::AddressSpace &space, void *destination, uptr source,
                                           usize size)
{
    static_cast<void>(smp::check_class_wait("copy_from_user under a spin lock"));
    auto once = [&] { return hw::copy_from_user(space, destination, source, size); };
    auto status = once();
    return status ? status : detail::recover(space, source, size, false, Attempt{once}, status);
}

[[nodiscard]] inline Status copy_to_user(const hw::AddressSpace &space, uptr destination, const void *source,
                                         usize size)
{
    static_cast<void>(smp::check_class_wait("copy_to_user under a spin lock"));
    auto once = [&] { return hw::copy_to_user(space, destination, source, size); };
    auto status = once();
    return status ? status : detail::recover(space, destination, size, true, Attempt{once}, status);
}

// Whether the range is accessible now, after resolving withheld pages for a
// write: the check a caller makes before committing to a later copy.
[[nodiscard]] inline bool user_range(const hw::AddressSpace &space, uptr address, usize size, bool write = false)
{
    static_cast<void>(smp::check_class_wait("user_range under a spin lock"));
    auto once = [&] {
        return hw::user_range(space, address, size, write) ? Status{} : Status::fault("user range is not accessible");
    };
    auto status = once();
    return status || detail::recover(space, address, size, write, Attempt{once}, status);
}

// The raw word pointer of hw::user_word, resolved like copy_to_user for a
// write. Valid only while the caller holds the owning mm.
[[nodiscard]] inline Result<u32 *> user_word(const hw::AddressSpace &space, uptr address, bool write)
{
    static_cast<void>(smp::check_class_wait("user_word under a spin lock"));
    Result<u32 *> word = Status::fault();
    auto once = [&]() -> Status {
        word = hw::user_word(space, address, write);
        return word ? Status{} : word.status();
    };
    auto status = once();
    if (!status && status.code() != StatusCode::invalid_argument)
        status = detail::recover(space, address, sizeof(u32), write, Attempt{once}, status);
    return status ? word : Result<u32 *>(status);
}

enum class WordAccess : u8
{
    read,
    write,
};

enum class WordResult : u8
{
    done,          // `use` ran on the word.
    misaligned,    // EINVAL.
    needs_resolve, // Not accessible now: withheld, unmapped or mid-replacement.
};

// One aligned user word, for the futex bucket: no fault, no lock, its own
// kernel window around the lookup and `use(u32 &word)`. Allowed under a class
// A lock. needs_resolve is not yet a fault: the caller drops its locks, takes
// mm, resolves (user_word or a copy) and retries; only an attempt that holds
// mm yields a genuine fault.
template <typename Use> [[nodiscard]] WordResult with_user_word(const hw::AddressSpace &space, uptr address,
                                                                WordAccess access, Use &&use)
{
    if (address % sizeof(u32))
        return WordResult::misaligned;
    const smp::KernelWindow window(space.root);
    auto word = hw::user_word(space, address, access == WordAccess::write);
    if (!word)
        return word.status().code() == StatusCode::invalid_argument ? WordResult::misaligned
                                                                    : WordResult::needs_resolve;
    std::forward<Use>(use)(*word.value());
    return WordResult::done;
}

} // namespace ok::user::uaccess
