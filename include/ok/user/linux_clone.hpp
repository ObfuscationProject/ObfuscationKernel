#pragma once

#include "ok/abi/linux_native.hpp"
#include "ok/core/kernel.hpp"
#include "ok/syscall/linux.hpp"

namespace ok::user::linux_bootstrap
{
namespace clone_flags
{
inline constexpr u32 vm = 0x100, fs = 0x200, files = 0x400, sighand = 0x800;
inline constexpr u32 thread = 0x10000, sysvsem = 0x40000, settls = 0x80000;
inline constexpr u32 parent_settid = 0x100000, child_cleartid = 0x200000;
inline constexpr u32 detached = 0x400000, child_settid = 0x1000000;
inline constexpr u32 shared_group = vm | fs | files | sighand | thread;
inline constexpr u32 supported = shared_group | sysvsem | settls | parent_settid | child_cleartid | detached | child_settid;
}

struct RejectThreadPreparation
{
    Status operator()(sched::ThreadControlBlock &) const
    {
        return Status::unsupported("native clone requires an explicit resource preparation hook");
    }
};

template <abi::linux_user::Profile Profile, typename PrepareThread>
i64 clone_thread(Kernel &kernel, sched::ProcessId pid, const arch::execution::UserFrame &parent,
                 const abi::linux_user::Call &call, PrepareThread &&prepare)
{
    namespace hw = arch::execution;
    namespace lu = abi::linux_user;
    using Bridge = lu::NativeBridge<Profile>;
    using Codec = lu::Codec<Profile>;
    const auto &a = call.arguments;
    const u32 flags = static_cast<u32>(a[0]);
    if ((flags & clone_flags::shared_group) != clone_flags::shared_group || (flags & ~clone_flags::supported))
        return -38;
    auto *process = kernel.scheduler().find(pid);
    const auto *caller = kernel.scheduler().current_thread();
    if (!process || !caller || caller->owner != pid || caller->state != sched::ProcessState::running)
        return -22;
    const auto &space = process->resources().native_space;
    // Linux copies a nonzero raw SP and inherits a zero SP. Mapping checks
    // belong to native entry/fault handling; musl x86_64 uses 8-mod-16 here.
    const uptr stack = a[1] ? static_cast<uptr>(a[1]) : parent.sp;
    constexpr bool backwards = Profile != lu::Profile::x86_64 && Profile != lu::Profile::loongarch64;
    const uptr tls_argument = static_cast<uptr>(a[backwards ? 3 : 4]);
    const uptr child_tid = static_cast<uptr>(a[backwards ? 4 : 3]);
    auto frame = parent;
    Bridge::set_stack_pointer(frame, stack);
    if (flags & clone_flags::settls)
    {
        uptr pointer = tls_argument;
        if constexpr (Profile == lu::Profile::i386)
        {
            std::array<u8, 16> bytes{};
            if (!hw::copy_from_user(space, bytes.data(), tls_argument, bytes.size())) return -14;
            auto word = [&](usize offset) {
                u32 value = 0;
                for (usize i = 0; i < 4; ++i) value |= static_cast<u32>(bytes[offset + i]) << (i * 8);
                return value;
            };
            // clone does not allocate a descriptor or write the user_desc back.
            if (word(0) != Bridge::i386_tls_entry || word(8) != 0xfffff || word(12) != 0x51) return -22;
            pointer = word(4);
        }
        if constexpr (Profile == lu::Profile::x86_64)
            if (pointer >= (u64{1} << 47) - hw::page_size) return -1; // Qualified four-level user TLS domain.
        if (!Bridge::set_thread_pointer(frame, {pointer}, Bridge::thread_pointer_route)) return -22;
    }
    if (!Bridge::encode_return(frame, {0, 0})) return -75;
    auto created = kernel.scheduler().create_thread_prepared(
        pid, kernel.arch().make_user_context({frame.pc, frame.sp}),
        [&](sched::ThreadControlBlock &child) -> Status {
            child.native_frame = frame;
            child.native_signal_mask = caller->native_signal_mask;
            child.native_active = true;
            child.tls.fs_base = frame.fs_base;
            child.tls.gs_base = frame.gs_base;
            child.tls.clear_tid_address = flags & clone_flags::child_cleartid ? child_tid : 0;
            child.tls.set_tid_address = flags & clone_flags::child_settid ? child_tid : 0;
            return std::forward<PrepareThread>(prepare)(child);
        });
    if (!created) return -syscall::ErrnoMapper::errno_for(created.status());
    if (flags & clone_flags::parent_settid)
    {
        std::array<u8, 4> bytes{};
        for (usize i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<u8>(created.value() >> (8 * (Codec::profile.big_endian ? 3 - i : i)));
        // Linux ignores this store failure after task creation. The child store
        // is deferred until that TCB's first user entry, before its continuation.
        static_cast<void>(hw::copy_to_user(space, static_cast<uptr>(a[2]), bytes.data(), bytes.size()));
    }
    return static_cast<i64>(created.value());
}
} // namespace ok::user::linux_bootstrap
