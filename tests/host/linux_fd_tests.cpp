#include "unavailable_realtime.hpp"
// Actual Linux adapter/common native copy boundary with hosted mappings only.
#define OK_SYSCALL_ORIGIN_MODEL 1
#define OK_NATIVE_VFS_CONTINUATION_MODEL 1
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
namespace mock
{
std::array<std::byte, 8192> memory{};
uptr forbidden{};
usize copies{};
struct Space { bool live{}; u32 generation{}; hw::MappingView pages; };
std::array<Space, 16> spaces; // Fixed model slot table.
uptr fail_destroy_root{};
usize live_spaces()
{
    usize count{};
    for (const auto &space : spaces) count += space.live;
    return count;
}
} // namespace mock
namespace ok::user::native
{
// Exercise the actual cold origin-preparation overflow refusal with a hosted
// boundary value; no native entry or image instructions run in this fixture.
struct SyscallOriginTestAccess
{
    template <lu::Profile P> static u64 exhaust(SyscallOriginTable<P> &table)
    { const auto previous = table.image_sequence_; table.image_sequence_ = ~u64{0}; return previous; }
    template <lu::Profile P> static void restore(SyscallOriginTable<P> &table, u64 previous) { table.image_sequence_ = previous; }
    template <lu::Profile P> static u64 sequence(const SyscallOriginTable<P> &table) { return table.image_sequence_; }
    template <lu::Profile P> static bool vacant(const SyscallOriginTable<P> &table, usize slot)
    {
        using Table = SyscallOriginTable<P>;
        const auto &record = table.records_[slot];
        return record.identity == typename Table::Identity{} && record.kind == Table::Kind::none &&
            record.family == Table::Adapter::Family::none && !record.live && !record.bound &&
            !record.linux_udp_borrows && !record.linux_udp_revoke_pending && !record.linux_udp_retire_pending &&
            record.linux_udp_operation == LinuxUdpImmediateOperation::none && !record.linux_udp_syscall &&
            record.tcp_operation == abi::linux_user::TcpCall::none && !record.tcp_syscall && !record.scheduler;
    }
};
struct VfsStatContinuationTestAccess
{
    struct Snapshot
    {
        memory::KernelEntryStackHandle lease{};
        u64 pid{}, tid{}, image{}, capture{}, epoch{};
        uptr root{};
        u32 space_slot{}, generation{};
        bool pending{}, captured{};
        [[nodiscard]] bool operator==(const Snapshot &) const = default;
    };
    template <lu::Profile P> static u64 exhaust(VfsStatContinuation<P> &table)
    { const auto previous = table.images_; table.images_ = ~u64{0}; return previous; }
    template <lu::Profile P> static void restore(VfsStatContinuation<P> &table, u64 previous) { table.images_ = previous; }
    template <lu::Profile P> static u64 sequence(const VfsStatContinuation<P> &table) { return table.images_; }
    template <lu::Profile P> static Snapshot snapshot(const VfsStatContinuation<P> &table, usize slot)
    {
        const auto &r = table.records_[slot];
        return {r.lease, r.pid, r.tid, r.image, r.capture, r.epoch, r.space.root, r.space.slot,
            r.space.generation, r.pending.has_value(), r.captured};
    }
};
}
namespace ok::arch
{
Status initialize_clock()
{
    return Status::success();
}
Result<u64> monotonic_nanoseconds()
{
    return u64{3000000042};
}
u64 clock_resolution_ns()
{
    return 1;
}
} // namespace ok::arch
namespace ok::arch::execution
{
bool supported() noexcept
{
    return false;
}
OwnershipReport ownership_report() noexcept { return {}; }
Status initialize()
{
    return Status::unsupported();
}
Result<AddressSpace> create_space(MappingView pages)
{
    for (usize index = 0; index < mock::spaces.size(); ++index)
    {
        auto &space = mock::spaces[index];
        if (space.live) continue;
        space.live = true; space.pages = pages; ++space.generation;
        return AddressSpace{index + 1, static_cast<u32>(index), space.generation};
    }
    return Status::no_memory("hosted loaded-image table slots exhausted");
}
Status destroy_space(AddressSpace &space)
{
    if (!space.root) return Status::success();
    if (space.root == mock::fail_destroy_root) return Status::busy("hosted loaded-image retirement refusal");
    CHECK(space.slot < mock::spaces.size());
    auto &saved = mock::spaces[space.slot];
    CHECK(saved.live && saved.generation == space.generation && space.root == space.slot + 1);
    saved.live = false; saved.pages = {};
    space = {};
    return Status::success();
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    auto result = enter(space, frame);
    if (result) result.value().kernel_stack_pointer = view.lower(); // Synthetic hosted capture only.
    return result;
}
Result<Trap> enter(AddressSpace &, UserFrame &)
{
    return Status::unsupported();
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
Syscall syscall(const UserFrame &)
{
    return {};
}
void set_return_value(UserFrame &f, i64 v)
{
    f.registers[0] = static_cast<uptr>(v);
}
bool user_range(const AddressSpace &, uptr address, usize size, bool)
{
    return !size || (address >= user_begin && address - user_begin < mock::memory.size() &&
                     size <= mock::memory.size() - (address - user_begin) &&
                     (!mock::forbidden || address >= mock::forbidden + 1 || size <= mock::forbidden - address));
}
Status copy_from_user(const AddressSpace &s, void *out, uptr address, usize size)
{
    ++mock::copies;
    if (!user_range(s, address, size, false))
        return Status::fault();
    if (size)
        std::memcpy(out, mock::memory.data() + address - user_begin, size);
    return Status::success();
}
Status copy_to_user(const AddressSpace &s, uptr address, const void *in, usize size)
{
    if (!user_range(s, address, size, true))
        return Status::fault();
    if (size)
        std::memcpy(mock::memory.data() + address - user_begin, in, size);
    return Status::success();
}
} // namespace ok::arch::execution


template <lu::Profile P>
void descriptors(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 4> slots)
{
    using Flags = lu::OpenCodec<P>;
    auto call = [&](lu::Operation operation, std::array<uptr, 4> arguments) {
        auto number = lu::Codec<P>::syscall_number(operation);
        CHECK(number);
        hw::UserFrame frame{};
        frame.registers[number_slot] = *number;
        for (usize i = 0; i < arguments.size(); ++i) frame.registers[slots[i]] = arguments[i];
        bool done = true;
        const i64 result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); });
        CHECK(done && user::linux_bootstrap::complete<P>(frame, result));
        auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        if (result >= 0) CHECK(returned.value().error == 0 && returned.value().value == static_cast<u64>(result));
        else CHECK(returned.value().error == lu::OpenCodec<P>::pathname_errno(static_cast<u32>(-result)));
        return result;
    };
    const auto operation = lu::Codec<P>::syscall_number(lu::Operation::fcntl64) ? lu::Operation::fcntl64 : lu::Operation::fcntl;
    auto fcntl = [&](uptr fd, uptr command, uptr argument = 0) { return call(operation, {fd, command, argument, 0}); };
    auto close = [&](uptr fd) { CHECK(call(lu::Operation::close, {fd, 0, 0, 0}) == 0); };
    constexpr uptr name = hw::user_begin + 128;
    std::memcpy(mock::memory.data() + 128, "/fd-file", 9);
    CHECK(call(lu::Operation::openat, {static_cast<uptr>(-100), name,
        2 | Flags::create | Flags::exclusive | Flags::cloexec | Flags::nofollow, 0600}) == 3);
    constexpr uptr fd = 3;
    const u32 base_flags = 2 | Flags::nofollow | (lu::Codec<P>::profile.word_bytes == 8 ? Flags::largefile : 0);
    CHECK(fcntl(fd, 3) == base_flags);
    CHECK(fcntl(fd, 1) == 1);
    CHECK(fcntl(fd, 2, 2) == 0 && fcntl(fd, 1) == 0);
    CHECK(fcntl(fd, 2, ~uptr{0}) == 0 && fcntl(fd, 1) == 1);
    CHECK(fcntl(fd, 0, 20) == 20 && fcntl(20, 1) == 0);
    CHECK(fcntl(fd, 1030, 21) == 21 && fcntl(21, 1) == 1);
    CHECK(fcntl(fd, 4, Flags::append | Flags::nonblock | Flags::create | Flags::cloexec | 1) == 0);
    CHECK(fcntl(20, 3) == (base_flags | Flags::append | Flags::nonblock));
    CHECK(fcntl(21, 3) == (base_flags | Flags::append | Flags::nonblock));
    CHECK(fcntl(20, 1) == 0 && fcntl(21, 1) == 1);
    for (u32 unsupported : {Flags::direct, Flags::noatime, Flags::asynchronous})
        CHECK(fcntl(fd, 4, unsupported) == -22 && fcntl(fd, 3) == (base_flags | Flags::append | Flags::nonblock));
    CHECK(fcntl(fd, 4, 0x80000000U) == 0 && fcntl(fd, 3) == base_flags);
    const auto before = kernel.scheduler().open_description_count();
    CHECK(call(lu::Operation::dup, {fd, 0, 0, 0}) == 4 && fcntl(4, 1) == 0);
    CHECK(kernel.scheduler().open_description_count() == before);
    const std::array<std::byte, 4> data{std::byte{'a'},std::byte{'b'},std::byte{'c'},std::byte{'d'}};
    CHECK(kernel.posix().write(fd, data).value() == 4);
    CHECK(kernel.posix().seek(fd, 0, posix::SeekWhence::set).value() == 0);
    std::array<std::byte, 1> byte{};
    CHECK(kernel.posix().read(4, byte).value() == 1 && byte[0] == std::byte{'a'});
    CHECK(kernel.posix().read(20, byte).value() == 1 && byte[0] == std::byte{'b'});
    CHECK(kernel.posix().read(fd, byte).value() == 1 && byte[0] == std::byte{'c'});
    CHECK(call(lu::Operation::dup3, {fd, 4, Flags::cloexec, 0}) == 4 && fcntl(4, 1) == 1);
    CHECK(call(lu::Operation::dup3, {fd, fd, 0, 0}) == -22);
    CHECK(call(lu::Operation::dup3, {static_cast<uptr>(-1), 4, Flags::append, 0}) == -22);
    CHECK(call(lu::Operation::dup3, {static_cast<uptr>(-1), static_cast<uptr>(-1), 0, 0}) == -22);
    CHECK(call(lu::Operation::dup3, {static_cast<uptr>(-1), 4, 0, 0}) == -9 && fcntl(4, 1) == 1);
    CHECK(call(lu::Operation::dup3, {fd, 32, 0, 0}) == -9);
    CHECK(call(lu::Operation::dup, {static_cast<uptr>(-1), 0, 0, 0}) == -9);
    if (lu::Codec<P>::syscall_number(lu::Operation::dup2)) {
        CHECK(call(lu::Operation::dup2, {fd, fd, 0, 0}) == 3 && fcntl(fd, 1) == 1);
        CHECK(call(lu::Operation::dup2, {fd, 4, 0, 0}) == 4 && fcntl(4, 1) == 0);
        CHECK(call(lu::Operation::dup2, {static_cast<uptr>(-1), static_cast<uptr>(-1), 0, 0}) == -9);
    }
    CHECK(fcntl(fd, 0, 32) == -22 && fcntl(fd, 1030, ~uptr{0}) == -22);
    CHECK(fcntl(~uptr{0}, 999) == -9 && fcntl(fd, 999) == -22);
    if (lu::Codec<P>::syscall_number(lu::Operation::fcntl))
        CHECK(call(lu::Operation::fcntl, {fd, 3, 0, 0}) == base_flags);
    if constexpr (lu::Codec<P>::profile.word_bytes == 8) {
        constexpr uptr high = uptr{1} << 32;
        CHECK(fcntl(high | fd, high | 3) == base_flags);
        CHECK(fcntl(fd, 1030, high | 22) == 22 && fcntl(22, 1) == 1);
        close(22);
        CHECK(call(lu::Operation::dup3, {high | fd, high | 4, high, 0}) == 4 && fcntl(4, 1) == 0);
    }
    std::array<uptr, 32> opened{};
    usize count = 0;
    for (;;) {
        const auto next = call(lu::Operation::dup, {fd, 0, 0, 0});
        if (next < 0) { CHECK(next == -24); break; }
        CHECK(count < opened.size());
        opened[count++] = static_cast<uptr>(next);
    }
    CHECK(count == 25 && fcntl(fd, 0, 0) == -24 && fcntl(fd, 1030, 0) == -24);
    CHECK(call(lu::Operation::dup3, {fd, 4, 0, 0}) == 4); // Replacement requires no free fd.
    close(opened[--count]);
    CHECK(fcntl(fd, 1030, 0) == static_cast<i64>(opened[count]));
    close(opened[count]);
    while (count) close(opened[--count]);
    close(4); close(20); close(21); close(fd);
    CHECK(call(lu::Operation::openat, {static_cast<uptr>(-100), name, Flags::largefile, 0}) == 3);
    CHECK(fcntl(3, 3) == Flags::largefile);
    close(3);
    CHECK(kernel.posix().unlink("/fd-file"));
    std::printf("PASS Linux descriptors %s: duplication, sharing, flags, scalar ABI, limits and rollback\n", lu::Codec<P>::profile.name);
}
template <lu::Profile P>
void shared_files(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 4> slots)
{
    using Memfd = lu::MemfdCodec<P>;
    auto raw = [&](u32 number, std::array<uptr, 4> arguments) {
        hw::UserFrame frame{};
        frame.registers[number_slot] = number;
        for (usize i = 0; i < arguments.size(); ++i) frame.registers[slots[i]] = arguments[i];
        bool done = true;
        const i64 result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); });
        CHECK(done && user::linux_bootstrap::complete<P>(frame, result));
        const auto returned = lu::NativeBridge<P>::decode_return(frame);
        CHECK(returned);
        if (result >= 0) CHECK(!returned.value().error && returned.value().value == static_cast<u64>(result));
        else {
            const u32 expected = result == -38 ? *lu::Codec<P>::errno_number(lu::Error::e_nosys)
                                              : static_cast<u32>(-result);
            CHECK(returned.value().error == lu::OpenCodec<P>::pathname_errno(expected));
        }
        return result;
    };
    auto call = [&](lu::Operation operation, std::array<uptr, 4> arguments) {
        const auto number = lu::Codec<P>::syscall_number(operation); CHECK(number);
        return raw(*number, arguments);
    };
    constexpr uptr name = hw::user_begin + 128, output = hw::user_begin + 256;
    std::memcpy(mock::memory.data() + 128, "English shared buffer", 22);
    CHECK(raw(Memfd::number, {name, 0, 0, 0}) == -38);
    CHECK(raw(Memfd::number, {name, 6, 0, 0}) == -22);
    CHECK(raw(Memfd::number, {hw::user_end, 2, 0, 0}) == -14);
    const auto fd = raw(Memfd::number, {name, 3, 0, 0}); CHECK(fd == 3);
    const auto slot = kernel.scheduler().find(pid)->fd_table().entry(3);
    CHECK(slot && slot->used);
    const auto handle = kernel.scheduler().open_description(slot->description)->native_shared_file;
    CHECK(handle && native::shared_files.info(handle).value().description);
    const auto operation = lu::Codec<P>::syscall_number(lu::Operation::fcntl64) ? lu::Operation::fcntl64 : lu::Operation::fcntl;
    auto fcntl = [&](uptr command, uptr argument = 0) { return call(operation, {3, command, argument, 0}); };
    CHECK(fcntl(1) == 1 && fcntl(Memfd::f_get_seals) == 0);
    const auto truncation = lu::Codec<P>::syscall_number(lu::Operation::ftruncate) ? lu::Operation::ftruncate : lu::Operation::ftruncate64;
    std::array<uptr, 4> truncate_arguments{3, 4096, 0, 0};
    if constexpr (lu::Codec<P>::profile.word_bytes == 4 && lu::Codec<P>::profile.aligned_64bit_pairs)
        if (truncation == lu::Operation::ftruncate64) truncate_arguments = {3, 0, 4096, 0};
    CHECK(call(truncation, truncate_arguments) == 0);
    CHECK(fcntl(Memfd::f_add_seals, Memfd::f_seal_grow | Memfd::f_seal_future_write) == -38);
    CHECK(fcntl(Memfd::f_get_seals) == 0);
    CHECK(fcntl(Memfd::f_add_seals, Memfd::f_seal_grow | Memfd::f_seal_shrink) == 0);
    CHECK(fcntl(Memfd::f_get_seals) == 6);
    CHECK(kernel.posix().pwrite(3, {reinterpret_cast<const std::byte *>("English"), 7}, 0).value() == 7);
    CHECK(call(lu::Operation::read, {3, output, 7, 0}) == 7);
    CHECK(std::memcmp(mock::memory.data() + 256, "English", 7) == 0);
    CHECK(call(lu::Operation::dup, {3, 0, 0, 0}) == 4);
    CHECK(kernel.posix().seek(4, 0, posix::SeekWhence::current).value() == 7);
    const auto frame = native::shared_files.page(handle, 0).value();
    CHECK(native::shared_files.retain_alias(frame));
    CHECK(fcntl(Memfd::f_add_seals, Memfd::f_seal_write | Memfd::f_seal_seal) == -16);
    CHECK(fcntl(Memfd::f_get_seals) == 6);
    CHECK(native::shared_files.release_alias(frame));
    CHECK(fcntl(Memfd::f_add_seals, Memfd::f_seal_write) == 0);
    CHECK(native::shared_files.retain_alias(frame));
    CHECK(fcntl(Memfd::f_add_seals, Memfd::f_seal_write) == 0);
    CHECK(fcntl(Memfd::f_get_seals) == 14);
    CHECK(call(lu::Operation::write, {3, name, 1, 0}) == -1);
    CHECK(call(lu::Operation::write, {3, name, 0, 0}) == 0);
    CHECK(kernel.posix().seek(4, 0, posix::SeekWhence::current).value() == 7);
    CHECK(native::shared_files.check_mapping_write(frame).code() == StatusCode::denied);
    CHECK(call(lu::Operation::close, {3, 0, 0, 0}) == 0);
    CHECK(native::shared_files.info(handle).value().description);
    CHECK(call(lu::Operation::close, {4, 0, 0, 0}) == 0);
    CHECK(!native::shared_files.info(handle).value().description && native::shared_files.info(handle).value().aliases == 1);
    CHECK(native::shared_files.release_alias(frame));
    CHECK(!native::shared_files.ledger().files && native::page_pool.user_count() == 0);
    std::printf("PASS Linux memfd adapter: profile=%s hosted=1 actual_native_alias=0 guest=0\n", lu::Codec<P>::profile.name);
}
void native_shared_syscalls(Kernel &kernel, sched::ProcessId pid)
{
    auto call = [&](syscall::Number number, std::array<uptr, 6> arguments) {
        bool complete = true;
        const auto result = native::dispatch(kernel, pid, {static_cast<uptr>(number), arguments}, complete);
        CHECK(complete); return result;
    };
    constexpr uptr name = hw::user_begin + 128, output = hw::user_begin + 256;
    std::memcpy(mock::memory.data() + 128, "Native English buffer", 22);
    CHECK(call(syscall::Number::memfd_create, {name, 3}) == 3);
    CHECK(call(syscall::Number::ftruncate, {3, 4096}) == 0);
    CHECK(call(syscall::Number::fcntl, {3, posix::f_ADD_SEALS, 6}) == 0);
    CHECK(call(syscall::Number::fcntl, {3, posix::f_GET_SEALS}) == 6);
    CHECK(kernel.posix().pwrite(3, {reinterpret_cast<const std::byte *>("Native"), 6}, 0).value() == 6);
    mock::forbidden = output + 3;
    CHECK(call(syscall::Number::read, {3, output, 6}) == -14);
    CHECK(kernel.posix().seek(3, 0, posix::SeekWhence::current).value() == 0);
    mock::forbidden = 0;
    CHECK(call(syscall::Number::read, {3, output, 6}) == 6);
    CHECK(std::memcmp(mock::memory.data() + 256, "Native", 6) == 0);
    CHECK(call(syscall::Number::fcntl, {3, posix::f_ADD_SEALS, 8}) == 0);
    constexpr uptr vector_address = hw::user_begin + 512;
    const native::PipeVector vectors[]{{name, 7}, {name, 5}};
    std::memcpy(mock::memory.data() + 512, vectors, sizeof vectors);
    mock::forbidden = name;
    const auto before_scalar = mock::copies;
    CHECK(call(syscall::Number::write, {3, name, 1}) == -1);
    CHECK(call(syscall::Number::pwrite64, {3, name, 1, 0}) == -1);
    CHECK(mock::copies == before_scalar); // Refuse before touching user payload.
    CHECK(call(syscall::Number::writev, {3, vector_address, 2}) == -1);
    CHECK(mock::copies == before_scalar + 1); // Only the vector list was copied.
    CHECK(kernel.posix().seek(3, 0, posix::SeekWhence::current).value() == 6);
    CHECK(call(syscall::Number::write, {3, name, 0}) == 0);
    CHECK(call(syscall::Number::pwrite64, {3, name, 0, 0}) == 0);
    const native::PipeVector empty[]{{0, 0}, {0, 0}};
    std::memcpy(mock::memory.data() + 512, empty, sizeof empty);
    CHECK(call(syscall::Number::writev, {3, vector_address, 2}) == 0);
    CHECK(kernel.posix().seek(3, 0, posix::SeekWhence::current).value() == 6);
    mock::forbidden = 0;
    std::array<std::byte, 6> preserved{};
    CHECK(kernel.posix().pread(3, preserved, 0).value() == preserved.size());
    CHECK(std::memcmp(preserved.data(), "Native", preserved.size()) == 0);
    CHECK(call(syscall::Number::fcntl, {3, posix::f_ADD_SEALS, 1}) == 0);
    CHECK(call(syscall::Number::fcntl, {3, posix::f_GET_SEALS}) == 15);
    CHECK(call(syscall::Number::close, {3}) == 0);
    CHECK(!native::shared_files.ledger().files && native::page_pool.user_count() == 0);
    std::puts("PASS native memfd syscall adapter: hosted=1 actual_native_alias=0 guest=0");
}
void loaded_image_cleanup(Kernel &kernel, sched::ProcessId pid)
{
    std::array<std::byte, 256> executable{};
    auto put = [&](usize offset, u64 value, usize width) {
        for (usize index = 0; index < width; ++index) executable[offset + index] = std::byte(value >> (8 * index));
    };
    put(0, 0x464c457f, 4); put(4, 2, 1); put(5, 1, 1); put(6, 1, 1);
    put(16, 2, 2); put(18, 62, 2); put(20, 1, 4);
    put(24, hw::user_begin + 128, 8); put(32, 64, 8);
    put(52, 64, 2); put(54, 56, 2); put(56, 1, 2);
    put(64, 1, 4); put(68, 5, 4); put(72, 0, 8); put(80, hw::user_begin, 8);
    put(96, executable.size(), 8); put(104, hw::page_size, 8); put(112, hw::page_size, 8);
    executable[128] = std::byte{0xcc}; // Diagnostic byte, never executed.
    const auto fd = kernel.posix().open("/loaded-cleanup", posix::o_CREAT | posix::o_WRONLY, 0755);
    CHECK(fd && kernel.posix().write(fd.value(), executable).value() == executable.size());
    CHECK(kernel.posix().close(fd.value()));
    auto *process = kernel.scheduler().find(pid); CHECK(process);
    auto &resources = process->resources();
    auto &thread = process->threads()[0];
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    CHECK(native::prepare_native_thread(thread));
    native::Arguments args;
    CHECK(args.argv.push_back(FixedString<256>("loaded-cleanup")));
    auto load = [&]() { return native::load(kernel, "/loaded-cleanup", args); };
    auto retire = [&](hw::AddressSpace identity) {
        native::Pages empty;
        CHECK(native::retire_space(identity, empty) && !identity.root);
        native::runtime_failure = {};
    };
    auto install_initial = [&] {
        auto initial = load(); CHECK(initial);
        const auto identity = initial.value().space;
        const auto frame = *initial.value().pages.frame(0);
        CHECK(initial.value().pages.size() == 17 && native::install(*process, initial.value()));
        CHECK(!initial.value().retirement.active() && initial.value().pages.empty() && !initial.value().space.root);
        CHECK(native::same_space(resources.native_space, identity) && resources.native_pages.frame(0)->generation == frame.generation);
    };
    install_initial();
    {
        auto previous = native::ImageRetirementReservation::reserve(); CHECK(previous);
        auto replacement = load(); CHECK(replacement && replacement.value().pages.size() == 17);
        const auto old_space = resources.native_space;
        const auto new_frame = *replacement.value().pages.frame(0);
        mock::fail_destroy_root = old_space.root;
        usize calls{};
        CHECK(native::retire_then_install(resources, replacement.value(), previous.value(), [&] {
            ++calls; return native::install(*process, replacement.value());
        }).code() == StatusCode::busy);
        CHECK(!calls && resources.native_pages.empty() && !resources.native_space.root);
        CHECK(!native::page_pool.valid(new_frame) && native::page_pool.pending_count() == 17);
        CHECK(mock::live_spaces() == 1 && native::page_pool.pinned_count() == 17);
        mock::fail_destroy_root = 0; retire(old_space);
        CHECK(!native::page_pool.used_count() && !mock::live_spaces());
    }
    install_initial();
    {
        auto previous = native::ImageRetirementReservation::reserve(); CHECK(previous);
        auto replacement = load(); CHECK(replacement && replacement.value().pages.size() == 17);
        const auto identity = replacement.value().space;
        std::array<memory::PhysicalFrame, 17> frames;
        for (usize index = 0; index < frames.size(); ++index) frames[index] = *replacement.value().pages.frame(index);
        const auto lease = thread.entry_stack;
        const auto stacks = native::entry_stacks.accounting();
        const auto vfs = native::VfsStatContinuationTestAccess::snapshot(native::vfs_continuations, lease.slot);
        CHECK(vfs.lease == lease && vfs.pid == thread.owner && vfs.tid == thread.tid && vfs.image &&
            !vfs.pending && !vfs.captured);
        thread.tls = {11, 12, 13, 14, 15, 16};
        thread.native_signal_mask = {{0x22, 0x33}};
        CHECK(thread.native_deadline.arm(10, 0, 7, false));
        process->set_cpu_affinity_mask(2);
        const auto previous_thread = thread;
        const auto previous_memory = resources.memory;
        const auto previous_personality = resources.personality;
        const auto previous_break_base = resources.program_break_base;
        const auto previous_break = resources.program_break;
        const auto previous_break_limit = resources.program_break_limit;
        const auto origin_sequence = native::SyscallOriginTestAccess::sequence(native::syscall_origins);
        const auto vfs_sequence = native::VfsStatContinuationTestAccess::exhaust(native::vfs_continuations);
        const auto allocations = native::page_pool.allocations();
        mock::fail_destroy_root = identity.root;
        usize calls{};
        CHECK(native::retire_then_install(resources, replacement.value(), previous.value(), [&] {
            ++calls; return native::install(*process, replacement.value(), {{0x44, 0x55}});
        }).code() == StatusCode::overflow);
        CHECK(calls == 1 && native::runtime_failure.code() == StatusCode::overflow);
        CHECK(native::VfsStatContinuationTestAccess::sequence(native::vfs_continuations) == ~u64{0});
        native::VfsStatContinuationTestAccess::restore(native::vfs_continuations, vfs_sequence);
        CHECK(native::SyscallOriginTestAccess::sequence(native::syscall_origins) == origin_sequence + 1);
        CHECK(native::SyscallOriginTestAccess::vacant(native::syscall_origins, lease.slot));
        CHECK(native::VfsStatContinuationTestAccess::snapshot(native::vfs_continuations, lease.slot) == vfs);
        CHECK(thread.entry_stack == lease && thread.kernel_stack_top == previous_thread.kernel_stack_top);
        CHECK(thread.native_active == previous_thread.native_active && thread.syscall_pending == previous_thread.syscall_pending);
        CHECK(thread.native_frame.registers == previous_thread.native_frame.registers &&
            thread.native_frame.pc == previous_thread.native_frame.pc && thread.native_frame.sp == previous_thread.native_frame.sp &&
            thread.native_frame.flags == previous_thread.native_frame.flags && thread.native_frame.fp_state == previous_thread.native_frame.fp_state &&
            thread.native_frame.fs_base == previous_thread.native_frame.fs_base && thread.native_frame.gs_base == previous_thread.native_frame.gs_base);
        CHECK(thread.tls.fs_base == 11 && thread.tls.gs_base == 12 && thread.tls.clear_tid_address == 13 &&
            thread.tls.set_tid_address == 14 && thread.tls.robust_list == 15 && thread.tls.robust_list_length == 16);
        CHECK(thread.native_signal_mask == previous_thread.native_signal_mask && thread.native_deadline.armed() &&
            thread.native_deadline.generation() == previous_thread.native_deadline.generation() &&
            thread.native_deadline.deadline() == previous_thread.native_deadline.deadline());
        CHECK(thread.wait_registration.ticket() == previous_thread.wait_registration.ticket() &&
            thread.wait_registration.active() == previous_thread.wait_registration.active() &&
            thread.wait_registration.completion() == previous_thread.wait_registration.completion());
        CHECK(resources.personality == previous_personality && resources.native_pages.empty() && !resources.native_space.root);
        CHECK(resources.program_break_base == previous_break_base && resources.program_break == previous_break &&
            resources.program_break_limit == previous_break_limit && process->cpu_affinity_mask() == 2);
        CHECK(resources.memory.area_count() == previous_memory.area_count());
        for (usize index = 0; index < previous_memory.area_count(); ++index) {
            const auto *before = previous_memory.area(index), *after = resources.memory.area(index);
            CHECK(before && after && before->base == after->base && before->length == after->length &&
                before->permissions == after->permissions);
        }
        const auto after_stacks = native::entry_stacks.accounting();
        CHECK(after_stacks.leased_slots == stacks.leased_slots && !after_stacks.active_slots &&
            after_stacks.quarantined_slots == stacks.quarantined_slots && after_stacks.reservations == stacks.reservations &&
            after_stacks.releases == stacks.releases);
        CHECK(native::entry_stacks.validate_inactive(lease, {thread.owner, thread.tid}));
        auto active = native::entry_stacks.activate_entry(lease, {thread.owner, thread.tid}, 0); CHECK(active);
        CHECK(native::syscall_origins.stage_capture(*process, thread, active.value(), kernel.scheduler()).status().code() == StatusCode::denied);
        CHECK(native::entry_stacks.deactivate_entry(lease, {thread.owner, thread.tid}, 0));
        CHECK(replacement.value().pages.empty() && !replacement.value().space.root && !replacement.value().retirement.active());
        native::QuarantinedSpace *saved{};
        for (auto &row : native::quarantined_spaces) if (row.used && native::same_space(row.space, identity)) saved = &row;
        CHECK(saved && saved->pages.size() == frames.size() && saved->pin_complete && saved->pinned_count == frames.size());
        for (usize index = 0; index < frames.size(); ++index) {
            const auto *token = saved->pages.frame(index); CHECK(token);
            CHECK(token->owner == frames[index].owner && token->address == frames[index].address &&
                token->index == frames[index].index && token->generation == frames[index].generation);
            CHECK(saved->pin_held(index) && saved->ownership(index) == native::QuarantinedSpace::Ownership::discharged);
        }
        CHECK(native::page_pool.allocations() == allocations && native::page_pool.pending_count() == 17 &&
            native::page_pool.pinned_count() == 17 && mock::live_spaces() == 1);
        mock::fail_destroy_root = 0; retire(identity);
        for (const auto &token : frames) CHECK(!native::page_pool.valid(token));
        CHECK(!native::page_pool.used_count() && !mock::live_spaces());
        thread.native_deadline.cancel();
    }
    install_initial();
    {
        auto previous = native::ImageRetirementReservation::reserve(); CHECK(previous);
        auto replacement = load(); CHECK(replacement);
        const auto identity = replacement.value().space;
        std::array<memory::PhysicalFrame, 17> frames;
        for (usize index = 0; index < frames.size(); ++index) frames[index] = *replacement.value().pages.frame(index);
        // Cleanup must need no new metadata even when every remaining purpose
        // cell is occupied after this real replacement has been loaded.
        std::array<native::NativePagePool::Lease, native::kernel_reserve_pages> pressure;
        usize held{};
        for (;;) { auto lease = native::page_pool.allocate_kernel_page(); if (!lease) break; pressure[held++] = std::move(lease.value()); }
        const auto allocations = native::page_pool.allocations();
        const auto sequence = native::SyscallOriginTestAccess::exhaust(native::syscall_origins);
        mock::fail_destroy_root = identity.root;
        usize calls{};
        CHECK(native::retire_then_install(resources, replacement.value(), previous.value(), [&] {
            ++calls; return native::install(*process, replacement.value());
        }).code() == StatusCode::overflow);
        native::SyscallOriginTestAccess::restore(native::syscall_origins, sequence);
        CHECK(calls == 1 && native::runtime_failure.code() == StatusCode::overflow);
        CHECK(replacement.value().pages.empty() && !replacement.value().space.root && !replacement.value().retirement.active());
        native::QuarantinedSpace *saved{};
        for (auto &row : native::quarantined_spaces) if (row.used && native::same_space(row.space, identity)) saved = &row;
        CHECK(saved && saved->pages.size() == frames.size() && saved->pin_complete && saved->pinned_count == frames.size());
        for (usize index = 0; index < frames.size(); ++index)
        {
            const auto *token = saved->pages.frame(index); CHECK(token);
            CHECK(token->owner == frames[index].owner && token->address == frames[index].address && token->generation == frames[index].generation);
            CHECK(saved->pin_held(index) && saved->ownership(index) == native::QuarantinedSpace::Ownership::discharged);
        }
        CHECK(native::page_pool.allocations() == allocations && native::page_pool.pending_count() == 17 && mock::live_spaces() == 1);
        mock::fail_destroy_root = 0; retire(identity);
        while (held) CHECK(native::page_pool.release_kernel_page(pressure[--held]));
        CHECK(!native::page_pool.used_count() && !mock::live_spaces());
    }
    {
        const auto allocations = native::page_pool.allocations();
        std::array<std::optional<native::ImageRetirementReservation>, native::quarantined_spaces.size()> held;
        for (auto &slot : held) { auto owner = native::ImageRetirementReservation::reserve(); CHECK(owner); slot.emplace(std::move(owner.value())); }
        CHECK(load().status().code() == StatusCode::no_memory);
        CHECK(native::page_pool.allocations() == allocations && !native::page_pool.used_count() && !mock::live_spaces());
        CHECK(resources.native_pages.empty() && !resources.native_space.root);
        for (auto &slot : held) slot.reset();
    }
    {
        std::array<native::NativePagePool::Lease, native::kernel_reserve_pages> pressure;
        usize held{};
        for (;;) { auto lease = native::page_pool.allocate_kernel_page(); if (!lease) break; pressure[held++] = std::move(lease.value()); }
        const auto users = native::page_pool.user_count(), objects = native::page_pool.object_count();
        CHECK(load().status().code() == StatusCode::no_memory);
        CHECK(native::page_pool.user_count() == users && native::page_pool.object_count() == objects && !mock::live_spaces());
        while (held) CHECK(native::page_pool.release_kernel_page(pressure[--held]));
    }
    const auto retired_lease = thread.entry_stack;
    CHECK(native::retire_native_thread(thread));
    {
        CHECK(!thread.entry_stack.pool && !thread.kernel_stack_top);
        // This local TCB has never been admitted into the scheduler. Preparing
        // its entry/origin state must not leave authority after VFS refusal.
        sched::ThreadControlBlock unpublished;
        unpublished.owner = thread.owner;
        unpublished.tid = thread.tid + 1000;
        const auto stacks = native::entry_stacks.accounting();
        const auto vfs = native::VfsStatContinuationTestAccess::snapshot(native::vfs_continuations, retired_lease.slot);
        const auto origin_sequence = native::SyscallOriginTestAccess::sequence(native::syscall_origins);
        const auto vfs_sequence = native::VfsStatContinuationTestAccess::exhaust(native::vfs_continuations);
        const auto allocations = native::page_pool.allocations();
        CHECK(native::prepare_native_thread(unpublished).code() == StatusCode::overflow);
        CHECK(native::VfsStatContinuationTestAccess::sequence(native::vfs_continuations) == ~u64{0});
        native::VfsStatContinuationTestAccess::restore(native::vfs_continuations, vfs_sequence);
        CHECK(native::SyscallOriginTestAccess::sequence(native::syscall_origins) == origin_sequence + 1);
        CHECK(native::SyscallOriginTestAccess::vacant(native::syscall_origins, retired_lease.slot));
        CHECK(native::VfsStatContinuationTestAccess::snapshot(native::vfs_continuations, retired_lease.slot) == vfs);
        CHECK(!unpublished.entry_stack.pool && !unpublished.kernel_stack_top);
        CHECK(!thread.entry_stack.pool && !thread.kernel_stack_top);
        const auto after_stacks = native::entry_stacks.accounting();
        CHECK(after_stacks.leased_slots == stacks.leased_slots && !after_stacks.active_slots &&
            after_stacks.quarantined_slots == stacks.quarantined_slots && after_stacks.reservations == stacks.reservations + 1 &&
            after_stacks.releases == stacks.releases + 1);
        CHECK(native::page_pool.allocations() == allocations && !native::page_pool.used_count() && !mock::live_spaces());
        CHECK(native::prepare_native_thread(unpublished));
        CHECK(unpublished.entry_stack.slot == retired_lease.slot && unpublished.entry_stack.generation > retired_lease.generation);
        CHECK(!kernel.scheduler().find_thread(unpublished.owner, unpublished.tid));
        CHECK(!native::entry_stacks.validate_inactive(retired_lease, {thread.owner, thread.tid}));
        CHECK(native::retire_native_thread(unpublished));
        CHECK(!unpublished.entry_stack.pool && !unpublished.kernel_stack_top);
        CHECK(native::SyscallOriginTestAccess::vacant(native::syscall_origins, retired_lease.slot));
        CHECK(native::entry_stacks.accounting().leased_slots == stacks.leased_slots);
    }
    thread.native_active = false;
    CHECK(kernel.posix().unlink("/loaded-cleanup"));
    for (const auto &saved : native::quarantined_spaces) CHECK(!saved.used && !saved.reserved);
    const auto ledger = native::shared_files.ledger();
    CHECK(!ledger.files && !ledger.aliases && !ledger.retiring_files && !ledger.retained_frames && !ledger.reserved_cells);
    CHECK(!native::page_pool.used_count() && !mock::live_spaces());
    std::puts("PASS loaded image exec ownership: actual_loader=1 hosted_backend=1 actual_instruction_execution=0 guest=0");
    std::puts("PASS native preparation cleanup: actual_loader=1 actual_install=1 actual_thread_prepare=1 origin_retired=1 vfs_overflow=1 exact_tokens=17 hosted_backend=1 guest=0");
}
int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    const auto context = kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096});
    auto made = kernel.scheduler().create_user_process("descriptor-adapter", context);
    CHECK(made && kernel.posix().activate(made.value()));
    const auto pid = made.value();
    CHECK(native::activate_pool() && native::shared_files.bind(native::page_pool));
    CHECK(kernel.scheduler().set_shared_memory_files(native::shared_files));
    native_shared_syscalls(kernel, pid);
    descriptors<lu::Profile::i386>(kernel, pid, 7, {4,6,5,1});
    descriptors<lu::Profile::x86_64>(kernel, pid, 14, {9,10,11,5});
    descriptors<lu::Profile::aarch64>(kernel, pid, 8, {0,1,2,3});
    descriptors<lu::Profile::arm32>(kernel, pid, 7, {0,1,2,3});
    descriptors<lu::Profile::rv64>(kernel, pid, 17, {10,11,12,13});
    descriptors<lu::Profile::rv32>(kernel, pid, 17, {10,11,12,13});
    descriptors<lu::Profile::loongarch64>(kernel, pid, 11, {4,5,6,7});
    descriptors<lu::Profile::mips>(kernel, pid, 2, {4,5,6,7});
    descriptors<lu::Profile::mips64>(kernel, pid, 2, {4,5,6,7});
    descriptors<lu::Profile::ppc>(kernel, pid, 0, {3,4,5,6});
    shared_files<lu::Profile::i386>(kernel, pid, 7, {4,6,5,1});
    shared_files<lu::Profile::x86_64>(kernel, pid, 14, {9,10,11,5});
    shared_files<lu::Profile::aarch64>(kernel, pid, 8, {0,1,2,3});
    shared_files<lu::Profile::arm32>(kernel, pid, 7, {0,1,2,3});
    shared_files<lu::Profile::rv64>(kernel, pid, 17, {10,11,12,13});
    shared_files<lu::Profile::rv32>(kernel, pid, 17, {10,11,12,13});
    shared_files<lu::Profile::loongarch64>(kernel, pid, 11, {4,5,6,7});
    shared_files<lu::Profile::mips>(kernel, pid, 2, {4,5,6,7});
    shared_files<lu::Profile::mips64>(kernel, pid, 2, {4,5,6,7});
    shared_files<lu::Profile::ppc>(kernel, pid, 0, {3,4,5,6});
    loaded_image_cleanup(kernel, pid);
}
