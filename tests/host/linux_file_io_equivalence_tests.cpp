#define OK_SYSCALL_ORIGIN_MODEL 1
#include "unavailable_realtime.hpp"
// Model coverage only: the Linux file-I/O and clock bodies kernel sections
// share with the kernel-lock path (ok/user/linux_file_io.hpp), and section
// admission (src/user/runtime/section.hpp), over all ten Linux profiles with a
// hosted mapping/copy backend. No CPU leaves the kernel lock, no section is
// pinned and no guest executes; the hardware lane is the OS SMP stress gate.
#include "native_runtime_units.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <utility>
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
namespace ok::user::native
{
// Gives a thread a live origin record, as its trapped Linux read leaves one.
struct SyscallOriginTestAccess
{
    static void live(SyscallOrigins &table, const sched::ThreadControlBlock &thread)
    {
        table.pool_ = const_cast<SyscallOrigins::Pool *>(thread.entry_stack.pool);
        auto &record = table.records_[thread.entry_stack.slot];
        record.identity.lease = thread.entry_stack;
        record.identity.pid = thread.owner;
        record.identity.tid = thread.tid;
        record.live = true;
    }
    static void clear(SyscallOrigins &table, const sched::ThreadControlBlock &thread, SyscallOrigins::Pool *pool)
    {
        table.records_[thread.entry_stack.slot] = {};
        table.pool_ = pool;
    }
    static SyscallOrigins::Pool *pool(SyscallOrigins &table) { return table.pool_; }
};
} // namespace ok::user::native
namespace mock
{
alignas(16) std::array<std::byte, 96 * 1024> memory{};
// The register the profile under test passes its first argument in, as the
// native trap decoder reports it (the A5 digest reads the descriptor there).
ok::usize first_argument_slot{};
// Runs once after the next copy to user memory: between two chunks of a read.
std::function<void()> after_copy_out;
} // namespace mock
namespace ok::arch
{
Status initialize_clock() { return Status::success(); }
Result<u64> monotonic_nanoseconds() { return u64{3000000042}; }
u64 clock_resolution_ns() { return 1; }
} // namespace ok::arch
namespace ok::arch::execution
{
bool supported() noexcept { return false; }
OwnershipReport ownership_report() noexcept { return {}; }
Status initialize() { return Status::unsupported(); }
Result<AddressSpace> create_space(MappingView) { return Status::unsupported(); }
Status destroy_space(AddressSpace &space)
{
    space = {};
    return Status::success();
}
Result<Trap> enter(AddressSpace &, UserFrame &) { return Status::unsupported(); }
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    return enter(space, frame);
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
Syscall syscall(const UserFrame &f)
{
    Syscall call{};
    call.arguments[0] = f.registers[mock::first_argument_slot];
    return call;
}
void set_return_value(UserFrame &f, i64 v) { f.registers[0] = static_cast<uptr>(v); }
bool user_range(const AddressSpace &, uptr address, usize size, bool)
{
    return !size || (address >= user_begin && address - user_begin < mock::memory.size() &&
                     size <= mock::memory.size() - (address - user_begin));
}
Status copy_from_user(const AddressSpace &s, void *out, uptr address, usize size)
{
    if (!user_range(s, address, size, false)) return Status::fault();
    if (size) std::memcpy(out, mock::memory.data() + address - user_begin, size);
    return Status::success();
}
Status copy_to_user(const AddressSpace &s, uptr address, const void *in, usize size)
{
    if (!user_range(s, address, size, true)) return Status::fault();
    if (size) std::memcpy(mock::memory.data() + address - user_begin, in, size);
    if (auto hook = std::exchange(mock::after_copy_out, nullptr)) hook();
    return Status::success();
}
Result<u32 *> user_word(const AddressSpace &s, uptr address, bool write)
{
    if (address % sizeof(u32)) return Status::invalid_argument();
    if (!user_range(s, address, sizeof(u32), write)) return Status::fault();
    return reinterpret_cast<u32 *>(mock::memory.data() + (address - user_begin));
}
} // namespace ok::arch::execution

namespace
{
// A regular file whose provider calls serialize themselves (as Ext4Node's
// operation gate does), so admission accepts it; `concurrent` false models
// every other provider.
class MemoryNode final : public fs::Node
{
  public:
    std::array<std::byte, 32 * 1024> bytes{};
    usize size{};
    usize syncs{};
    bool concurrent{true};
    std::string_view name() const override { return "file-io-model"; }
    fs::Metadata metadata() const override
    {
        fs::Metadata info{};
        info.type = fs::NodeType::regular;
        info.size = size;
        return info;
    }
    Result<fs::FileBuffer> read(usize offset, usize count) const override
    {
        fs::FileBuffer out{};
        if (offset >= size) return out;
        out.size = count < size - offset ? count : size - offset;
        if (out.size > out.data.size()) out.size = out.data.size();
        std::memcpy(out.data.data(), bytes.data() + offset, out.size);
        return out;
    }
    Status write(usize offset, std::span<const std::byte> data) override
    {
        if (offset > bytes.size() || data.size() > bytes.size() - offset) return Status::no_space();
        std::memcpy(bytes.data() + offset, data.data(), data.size());
        if (offset + data.size() > size) size = offset + data.size();
        return Status::success();
    }
    Status sync() override
    {
        ++syncs;
        return Status::success();
    }
    bool concurrent_io() const override { return concurrent; }
    fs::Node *lookup(std::string_view) override { return nullptr; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
};

constexpr uptr stack = hw::user_begin + 1024;
constexpr uptr input = hw::user_begin + 4096, output = hw::user_begin + 32768, record = hw::user_begin + 256;

template <lu::Profile P> struct Model
{
    Kernel &kernel;
    sched::ProcessId pid;
    usize number_slot;
    std::array<usize, 6> slots;

    // The frame of `operation` with `args`; a 64-bit offset at `wide` goes
    // into the profile's register pair. MIPS O32 passes words 4 and 5 on
    // the user stack.
    hw::UserFrame frame(lu::Operation operation, std::array<u64, 6> args, int wide = -1) const
    {
        using Codec = lu::Codec<P>;
        auto number = Codec::syscall_number(operation);
        if (!number) std::fprintf(stderr, "%s: no number for operation %u\n", Codec::profile.name, static_cast<unsigned>(operation));
        CHECK(number);
        hw::UserFrame f{};
        f.sp = stack;
        f.registers[number_slot] = *number;
        if (wide >= 0 && Codec::profile.word_bytes == 4)
        {
            const u64 value = args[static_cast<usize>(wide)];
            const usize first = Codec::profile.aligned_64bit_pairs ? 4 : 3;
            args[static_cast<usize>(wide)] = 0;
            args[first] = Codec::profile.big_endian ? value >> 32 : static_cast<u32>(value);
            args[first + 1] = Codec::profile.big_endian ? static_cast<u32>(value) : value >> 32;
        }
        for (usize i = 0; i < 6; ++i)
        {
            if constexpr (P == lu::Profile::mips)
                if (i >= 4)
                {
                    for (usize byte = 0; byte < 4; ++byte)
                        mock::memory[1024 + i * 4 + byte] = static_cast<std::byte>(args[i] >> (8 * (3 - byte)));
                    continue;
                }
            f.registers[slots[i]] = static_cast<uptr>(args[i]);
        }
        return f;
    }
    // The kernel-lock path: the Linux dispatcher and the native boundary.
    i64 locked(const hw::UserFrame &call)
    {
        auto f = call;
        bool done = true;
        const auto result = user::linux_bootstrap::dispatch<P>(kernel, pid, f, done,
            [&](hw::Syscall request, bool &complete) { return native::dispatch(kernel, pid, request, complete); });
        CHECK(done && user::linux_bootstrap::complete<P>(f, result));
        return result;
    }
    sched::ThreadControlBlock &thread() { return kernel.scheduler().find(pid)->threads()[0]; }
    // Admission with the frame pending on the process's thread.
    native::Admission admit(const hw::UserFrame &call, native::SectionCall<P> &admitted)
    {
        auto &t = thread();
        t.native_frame = call;
        t.syscall_pending = true;
        const auto result = native::admit_section<P>(kernel, *kernel.scheduler().find(pid), t, 0, admitted);
        t.syscall_pending = false;
        return result;
    }
    // The section path: admitted, then the body with the caller bound, as
    // run_section does without leaving the (hosted, absent) kernel lock.
    i64 section(const hw::UserFrame &call)
    {
        native::SectionCall<P> admitted{};
        if (admit(call, admitted) != native::Admission::run)
        {
            for (usize reason = 0; reason < smp::section_decline_count; ++reason)
                std::fprintf(stderr, "%.*s=%zu ", static_cast<int>(smp::section_decline_names[reason].size()),
                             smp::section_decline_names[reason].data(), static_cast<usize>(smp::section_cpus[0].declines[reason].load()));
            std::fprintf(stderr, "\n%s: section declined\n", lu::Codec<P>::profile.name);
            std::abort();
        }
        return body(admitted);
    }
    i64 body(native::SectionCall<P> &admitted)
    {
        auto &posix = kernel.posix();
        native::SectionAccess access{posix, kernel.scheduler(), kernel.scheduler().find(pid)->resources()};
        posix.bind_section_caller(0, *admitted.process, *admitted.thread);
        const auto result = native::section_work<P>(access, admitted);
        posix.unbind_section_caller(0);
        return result;
    }
    // A decline, its reason counted once and nothing else changed.
    void declines(const hw::UserFrame &call, smp::SectionDecline reason)
    {
        auto &self = smp::section_cpus[0];
        const auto before = self.declines[static_cast<usize>(reason)].load();
        const auto declined = self.declined.load();
        native::SectionCall<P> admitted{};
        CHECK(admit(call, admitted) == native::Admission::decline);
        CHECK(self.declines[static_cast<usize>(reason)].load() == before + 1 && self.declined.load() == declined + 1);
        CHECK(self.purity_faults.load() == 0);
    }
};

usize offset_of(Kernel &kernel, sched::ProcessId pid, posix::Fd fd)
{
    const auto *slot = kernel.scheduler().find(pid)->fd_table().entry(fd);
    return kernel.scheduler().open_description(slot->description)->offset;
}

template <lu::Profile P>
void equivalence(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 6> slots)
{
    using O = lu::Operation;
    using D = smp::SectionDecline;
    Model<P> model{kernel, pid, number_slot, slots};
    mock::first_argument_slot = slots[0];
    auto &process = *kernel.scheduler().find(pid);
    auto &t = model.thread();
    const auto saved_state = t.state;
    const bool saved_active = t.native_active;
    const auto saved_personality = process.resources().personality;
    t.state = sched::ProcessState::running;
    t.native_active = true;
    process.resources().personality = sched::UserPersonality::linux;
    // Two descriptions with equal files: the section path uses `a`, the
    // kernel-lock path `b`, call for call. The RAM nodes are swapped for
    // the model nodes in this serialized model and restored before closing.
    auto a = kernel.posix().open("/file-io-a", posix::o_CREAT | posix::o_TRUNC | posix::o_RDWR, 0600);
    auto b = kernel.posix().open("/file-io-b", posix::o_CREAT | posix::o_TRUNC | posix::o_RDWR, 0600);
    CHECK(a && b);
    auto description = [&](posix::Fd fd) {
        return kernel.scheduler().open_description(process.fd_table().entry(fd)->description);
    };
    MemoryNode na, nb;
    auto *ra = description(a.value())->node, *rb = description(b.value())->node;
    description(a.value())->node = &na;
    description(b.value())->node = &nb;
    for (usize i = 0; i < 20000; ++i)
        mock::memory[input - hw::user_begin + i] = static_cast<std::byte>('a' + i % 26);
    const auto fa = static_cast<u64>(a.value()), fb = static_cast<u64>(b.value());
    // One call on each path; results, positions and files must agree.
    auto both = [&](O operation, std::array<u64, 6> args, int wide = -1) {
        auto sa = args, sb = args;
        sa[0] = fa;
        sb[0] = fb;
        std::array<std::byte, 20000> out_a{};
        std::fill(mock::memory.begin() + (output - hw::user_begin), mock::memory.begin() + (output - hw::user_begin) + 20000, std::byte{0});
        const auto ra_ = model.section(model.frame(operation, sa, wide));
        std::memcpy(out_a.data(), mock::memory.data() + (output - hw::user_begin), out_a.size());
        std::fill(mock::memory.begin() + (output - hw::user_begin), mock::memory.begin() + (output - hw::user_begin) + 20000, std::byte{0});
        const auto rb_ = model.locked(model.frame(operation, sb, wide));
        CHECK(ra_ == rb_);
        CHECK(!std::memcmp(out_a.data(), mock::memory.data() + (output - hw::user_begin), out_a.size()));
        CHECK(offset_of(kernel, pid, a.value()) == offset_of(kernel, pid, b.value()));
        CHECK(na.size == nb.size && !std::memcmp(na.bytes.data(), nb.bytes.data(), na.size));
        CHECK(na.syncs == nb.syncs);
        return ra_;
    };
    // lseek, or on a profile with only _llseek (rv32, which never runs
    // sections) the shared helper through the kernel-lock path on both.
    constexpr bool has_lseek = lu::Codec<P>::syscall_number(O::lseek).has_value();
    auto seek = [&](i64 offset, u64 whence) -> i64 {
        if constexpr (has_lseek)
            return both(O::lseek, {0, static_cast<u64>(offset), whence});
        std::array<i64, 2> results{};
        for (usize k = 0; k < 2; ++k)
        {
            const auto fd = k ? fb : fa;
            const auto raw = static_cast<u64>(offset);
            results[k] = model.locked(model.frame(O::_llseek, {fd, raw >> 32, static_cast<u32>(raw), record, whence}));
            if (results[k] == 0)
            {
                u64 value{};
                for (usize i = 0; i < 8; ++i)
                {
                    const auto byte = std::to_integer<u64>(mock::memory[record - hw::user_begin + i]);
                    value |= byte << (8 * (lu::Codec<P>::profile.big_endian ? 7 - i : i));
                }
                results[k] = static_cast<i64>(value);
            }
        }
        CHECK(results[0] == results[1] && offset_of(kernel, pid, a.value()) == offset_of(kernel, pid, b.value()));
        return results[0];
    };
    // Expectations the kernel-lock path had before the extraction.
    CHECK(both(O::write, {0, input, 5}) == 5);
    CHECK(both(O::write, {0, input + 5, 8192}) == 4096);
    CHECK(seek(0, 0) == 0);
    CHECK(both(O::read, {0, output, 3}) == 3 && mock::memory[output - hw::user_begin] == std::byte{'a'});
    CHECK(both(O::read, {0, output, 9000}) == 4098);
    CHECK(both(O::read, {0, output, 7}) == 0);
    CHECK(seek(0, 2) == 4101);
    CHECK(seek(-5, 1) == 4096);
    CHECK(both(O::write, {0, input, 4096}) == 4096 && na.size == 8192);
    CHECK(seek(1, 0) == 1);
    CHECK(both(O::read, {0, output, 12000}) == 8191);
    CHECK(seek(-1, 0) == -22);
    CHECK(both(O::pread64, {0, output, 6, 2}, 3) == 6 && mock::memory[output - hw::user_begin] == std::byte{'c'});
    CHECK(both(O::pread64, {0, output, 6, 9000}, 3) == 0);
    CHECK(both(O::pwrite64, {0, input + 1, 3, 8190}, 3) == 3 && na.size == 8193);
    CHECK(both(O::pread64, {0, output, 9000, 0}, 3) == 4096);
    CHECK(both(O::fsync, {0}) == 0 && na.syncs == 1);
    CHECK(both(O::fdatasync, {0}) == 0 && na.syncs == 2);
    CHECK(both(O::write, {0, input, 0}) == 0 && both(O::read, {0, output, 0}) == 0);
    // C1: identity and the clock record, with the hosted clocks.
    const auto pid_locked = model.locked(model.frame(O::getpid, {}));
    const auto tid_locked = model.locked(model.frame(O::gettid, {}));
    CHECK(pid_locked == static_cast<i64>(pid) && tid_locked == static_cast<i64>(t.tid));
    {
        const auto shadows = smp::section_cpus[0].shadow_checked.load();
        for (usize i = 0; i < 2 * native::section_shadow_period; ++i)
        {
            native::SectionCall<P> admitted{};
            const auto operation = i % 2 ? O::gettid : O::getpid;
            const auto admission = model.admit(model.frame(operation, {}), admitted);
            if (admission == native::Admission::decline)
            {
                // A shadow: the kernel-lock path's answer is compared.
                native::shadow_compare(0, pid, t.tid, model.locked(model.frame(operation, {})));
                continue;
            }
            CHECK(model.body(admitted) == (operation == O::getpid ? pid_locked : tid_locked));
        }
        CHECK(smp::section_cpus[0].shadow_checked.load() > shadows && smp::section_cpus[0].shadow_mismatched.load() == 0);
    }
    for (const auto operation : {O::clock_gettime, O::clock_getres})
    {
        if (!lu::Codec<P>::syscall_number(operation)) continue;
        for (u64 clock : {u64{1}})
        {
            std::array<std::byte, 16> section_record{};
            const auto result = model.section(model.frame(operation, {clock, record}));
            std::memcpy(section_record.data(), mock::memory.data() + (record - hw::user_begin), 16);
            std::memset(mock::memory.data() + (record - hw::user_begin), 0, 16);
            CHECK(result == model.locked(model.frame(operation, {clock, record})) && result == 0);
            CHECK(!std::memcmp(section_record.data(), mock::memory.data() + (record - hw::user_begin), 16));
        }
    }
    // Admission declines, in precedence order, each with no effect (A5 with
    // OK_LOCK_DEBUG); the kernel-lock path then answers the call.
    model.declines(model.frame(O::read, {~u64{0} >> 33, output, 1}), D::descriptor);
    CHECK(model.locked(model.frame(O::read, {~u64{0} >> 33, output, 1})) == -9);
    model.declines(model.frame(O::read, {~u64{0} >> 33, output, 1 << 20}), D::descriptor);
    if constexpr (has_lseek)
    {
        model.declines(model.frame(O::lseek, {~u64{0} >> 33, 0, 3}), D::descriptor);
        model.declines(model.frame(O::lseek, {fa, 0, 3}), D::size);
        CHECK(model.locked(model.frame(O::lseek, {fb, 0, 3})) == -22);
    }
    model.declines(model.frame(O::read, {fa, output, native::section_io_limit + 1}), D::size);
    model.declines(model.frame(O::read, {fa, 0, 1}), D::fault);
    CHECK(model.locked(model.frame(O::read, {fb, 0, 1})) == -14);
    model.declines(model.frame(O::pread64, {fa, output, 1, static_cast<u64>(-1)}, 3), D::size);
    CHECK(model.locked(model.frame(O::pread64, {fb, output, 1, static_cast<u64>(-1)}, 3)) == -22);
    CHECK(kernel.posix().fcntl(a.value(), posix::f_SETFL, posix::o_APPEND));
    model.declines(model.frame(O::write, {fa, input, 1}), D::append);
    model.declines(model.frame(O::write, {fa, input, 1 << 20}), D::append);
    model.declines(model.frame(O::pwrite64, {fa, input, 1, 0}, 3), D::append);
    CHECK(kernel.posix().fcntl(a.value(), posix::f_SETFL, 0));
    na.concurrent = false;
    model.declines(model.frame(O::read, {fa, 0, 1}), D::kind);
    model.declines(model.frame(O::fsync, {fa}), D::kind);
    na.concurrent = true;
    model.declines(model.frame(O::getppid, {}), D::number);
    t.console_wait_epoch = 1;
    model.declines(model.frame(O::read, {~u64{0} >> 33, output, 1}), D::console);
    t.console_wait_epoch = 0;
    process.resources().personality = sched::UserPersonality::native;
    model.declines(model.frame(O::getppid, {}), D::personality);
    process.resources().personality = sched::UserPersonality::linux;
    native::started = false;
    model.declines(model.frame(O::fsync, {fa}), D::runtime);
    native::started = true;
    smp::close_section_admission();
    model.declines(model.frame(O::getppid, {}), D::shutdown);
    smp::open_section_admission();
    if constexpr (P == lu::Profile::x86_64)
    {
        // A5 with a section working on another CPU: that section may share
        // the description (fork) and move its offset, which is then not a
        // decline's effect; with none working, an offset change is caught.
        auto *shared = description(a.value());
        t.native_frame = model.frame(O::read, {fa, output, 1});
        const auto quiet = native::admission_digest(kernel, process, t);
        CHECK(quiet.offset_stable && native::admission_digest(kernel, process, t).equal(quiet, true));
        shared->offset += 7;
        CHECK(!native::admission_digest(kernel, process, t).equal(quiet, true));
        shared->offset -= 7;
        smp::section_records[1].store(smp::SectionState::working);
        const auto busy = native::admission_digest(kernel, process, t);
        CHECK(!busy.offset_stable);
        shared->offset += 7;
        smp::section_records[1].store(smp::SectionState::returning);
        CHECK(native::admission_digest(kernel, process, t).equal(busy, true));
        shared->flags ^= posix::o_NONBLOCK;
        CHECK(!native::admission_digest(kernel, process, t).equal(busy, true));
        shared->flags ^= posix::o_NONBLOCK;
        smp::section_records[1].store(smp::SectionState::idle);
        shared->offset -= 7;

        // A5 with a section of the process pinned: a drain during admission
        // (the copy-on-write resolver's) commits it, and the commit revokes
        // its thread's origin record. That is not a decline's effect; the
        // same change to an unpinned thread's record is. The model's one
        // thread stands in for the pinned sibling.
        {
            using Access = native::SyscallOriginTestAccess;
            auto *pool = Access::pool(native::syscall_origins);
            Access::live(native::syscall_origins, t);
            CHECK(native::syscall_origins.thread_ownership(t).live == 1);
            auto &sibling = smp::section_records[1];
            sibling.pid = t.owner;
            sibling.tid = t.tid;
            sibling.store(smp::SectionState::returning);
            const auto pinned = native::admission_digest(kernel, process, t);
            native::syscall_origins.revoke(t);
            sibling.store(smp::SectionState::committed);
            CHECK(native::admission_digest(kernel, process, t).equal(pinned, false));
            sibling.store(smp::SectionState::idle);
            Access::live(native::syscall_origins, t);
            const auto unpinned = native::admission_digest(kernel, process, t);
            native::syscall_origins.revoke(t);
            CHECK(!native::admission_digest(kernel, process, t).equal(unpinned, false));
            sibling.pid = 0;
            sibling.tid = 0;
            Access::clear(native::syscall_origins, t, pool);
        }

        // A section re-entering a position lock this CPU holds reports a
        // self-deadlock and a section assertion instead of spinning.
        auto *lock = kernel.scheduler().position_lock(shared);
        CHECK(lock != nullptr);
        const auto asserts = smp::section_cpus[0].section_asserts.load();
        const auto violations = smp::lock_violation_count(0);
        smp::set_cpu_mode(0, smp::mode_section);
        {
            const sched::DescriptionPositionGuard outer(lock);
            const sched::DescriptionPositionGuard inner(lock);
            CHECK(lock->owned_by_current_cpu());
        }
        smp::clear_cpu_mode(0, smp::mode_section);
        CHECK(!lock->owned_by_current_cpu() && lock->try_lock());
        lock->unlock();
        CHECK(smp::section_cpus[0].section_asserts.load() == asserts + 1 &&
              smp::lock_violation_count(0) == violations + 1);
        smp::section_cpus[0].section_asserts.store(asserts);

        // A read staged in several chunks holds the description's position
        // across all of them and the copy-outs between them, on both paths
        // and without re-entering the lock: a reader on another CPU that
        // arrives between two chunks reads after the whole read.
        for (const bool sectioned : {false, true})
        {
            constexpr usize chunk = sched::pipe_capacity;
            na.size = 3 * chunk;
            for (usize i = 0; i < na.size; ++i)
                na.bytes[i] = static_cast<std::byte>(i / chunk * 64 + i % 61);
            shared->offset = 0;
            std::fill(mock::memory.begin() + (output - hw::user_begin),
                      mock::memory.begin() + (output - hw::user_begin) + 2 * chunk, std::byte{0});
            std::atomic<bool> entered{false}, done{false};
            Result<usize> second = usize{0};
            std::array<std::byte, chunk> second_out{};
            std::thread reader;
            mock::after_copy_out = [&] {
                // A section of the same process on CPU 1 reads the description.
                reader = std::thread([&] {
                    const smp::ScopedHostedCpu cpu(1);
                    smp::set_cpu_mode(1, smp::mode_section);
                    kernel.posix().bind_section_caller(1, process, t);
                    entered.store(true);
                    second = kernel.posix().read(a.value(), second_out);
                    kernel.posix().unbind_section_caller(1);
                    smp::clear_cpu_mode(1, smp::mode_section);
                    done.store(true);
                });
                while (!entered.load()) std::this_thread::yield();
                // Room for the second reader to take the position if it were free.
                for (int spin = 0; spin < 200000 && !done.load(); ++spin) std::this_thread::yield();
            };
            const auto staged_asserts = smp::section_cpus[0].section_asserts.load();
            const auto staged_violations = smp::lock_violation_count(0) + smp::lock_violation_count(1);
            const auto call = model.frame(O::read, {fa, output, 2 * chunk});
            i64 first{};
            if (sectioned)
            {
                native::SectionCall<lu::Profile::x86_64> admitted{};
                CHECK(model.admit(call, admitted) == native::Admission::run);
                smp::set_cpu_mode(0, smp::mode_section);
                first = model.body(admitted);
                smp::clear_cpu_mode(0, smp::mode_section);
            }
            else
                first = model.locked(call);
            CHECK(!mock::after_copy_out && reader.joinable());
            reader.join();
            CHECK(first == static_cast<i64>(2 * chunk) && second && second.value() == chunk);
            CHECK(!std::memcmp(mock::memory.data() + (output - hw::user_begin), na.bytes.data(), 2 * chunk));
            CHECK(!std::memcmp(second_out.data(), na.bytes.data() + 2 * chunk, chunk));
            CHECK(shared->offset == 3 * chunk && !lock->owned_by_current_cpu());
            CHECK(smp::section_cpus[0].section_asserts.load() == staged_asserts &&
                  smp::lock_violation_count(0) + smp::lock_violation_count(1) == staged_violations);
        }
    }
    // Teardown: the RAM nodes return before their descriptions close.
    description(a.value())->node = ra;
    description(b.value())->node = rb;
    CHECK(kernel.posix().close(a.value()) && kernel.posix().close(b.value()));
    CHECK(kernel.posix().unlink("/file-io-a") && kernel.posix().unlink("/file-io-b"));
    t.state = saved_state;
    t.native_active = saved_active;
    process.resources().personality = saved_personality;
    std::printf("PASS Linux file I/O equivalence %s: section bodies equal the kernel-lock path; admission declines "
                "in order with no effect\n", lu::Codec<P>::profile.name);
}
} // namespace

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    auto made = kernel.scheduler().create_user_process("file-io-model",
        kernel.arch().make_user_context({hw::user_begin, hw::user_end - 4096}));
    CHECK(made && kernel.posix().activate(made.value()));
    const auto pid = made.value();
    native::started = true;
    smp::open_section_admission();
    equivalence<lu::Profile::i386>(kernel, pid, 7, {4, 6, 5, 1, 0, 2});
    equivalence<lu::Profile::x86_64>(kernel, pid, 14, {9, 10, 11, 5, 7, 6});
    equivalence<lu::Profile::aarch64>(kernel, pid, 8, {0, 1, 2, 3, 4, 5});
    equivalence<lu::Profile::arm32>(kernel, pid, 7, {0, 1, 2, 3, 4, 5});
    equivalence<lu::Profile::rv64>(kernel, pid, 17, {10, 11, 12, 13, 14, 15});
    equivalence<lu::Profile::rv32>(kernel, pid, 17, {10, 11, 12, 13, 14, 15});
    equivalence<lu::Profile::loongarch64>(kernel, pid, 11, {4, 5, 6, 7, 8, 9});
    equivalence<lu::Profile::mips>(kernel, pid, 2, {4, 5, 6, 7, 0, 0});
    equivalence<lu::Profile::mips64>(kernel, pid, 2, {4, 5, 6, 7, 8, 9});
    equivalence<lu::Profile::ppc>(kernel, pid, 0, {3, 4, 5, 6, 7, 8});
    native::started = false;
}
