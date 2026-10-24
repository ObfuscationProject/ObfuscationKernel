// Canonical scheduler/origin implementation with owned synthetic trap/usercopy.
#include "../../src/user/linux_udp_immediate_access.hpp"
#include "../../src/user/linux_udp_receive_authority.hpp"
#include "../../src/user/native_shared_capture.hpp"
#include "ok/user/native_entry_stacks.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>

using namespace ok;
namespace native = user::native;
namespace hw = arch::execution;
namespace lu = abi::linux_user;
static usize checks{}, profile_cases{}, backend_entries{}, input_copies{}, output_copies{}, mapping_queries{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "LINUX_UDP_ACCESS_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)

namespace model
{
enum class Action : u8 { none, rotate_epoch, close_and_reuse, completed_wait, redispatch };
struct Context
{
    memory::KernelEntryStackPool *pool{};
    sched::Scheduler *scheduler{};
    net::NativeUdpService *service{};
    sched::ProcessId pid{};
    lu::Profile profile{};
    hw::AddressSpace space{};
    bool bad_stack{}, bad_trap{}, copy_fault{};
    Action action{Action::none};
    u32 replacement_endpoint{};
    std::array<std::byte, 64> memory{};
} context;
hw::Trap trap()
{
    if (context.bad_trap) return {hw::TrapKind::timer};
    switch (context.profile)
    {
    case lu::Profile::i386: return {hw::TrapKind::syscall, 128};
    case lu::Profile::x86_64: return {hw::TrapKind::syscall, 256};
    case lu::Profile::aarch64: return {hw::TrapKind::syscall, 0x15, 0x54000000};
    case lu::Profile::arm32: return {hw::TrapKind::syscall, 2};
    case lu::Profile::rv64: case lu::Profile::rv32: return {hw::TrapKind::syscall, 8, 8};
    case lu::Profile::loongarch64: return {hw::TrapKind::syscall, 11};
    case lu::Profile::mips: case lu::Profile::mips64: return {hw::TrapKind::syscall, 8, 32};
    default: return {hw::TrapKind::syscall, 8};
    }
}
void after_copy()
{
    const auto action = std::exchange(context.action, Action::none);
    auto &scheduler = *context.scheduler;
    auto *thread = scheduler.find_thread(context.pid, context.pid); CHECK(thread);
    if (action == Action::rotate_epoch)
    {
        auto view = context.pool->activate_entry(thread->entry_stack, {context.pid, context.pid}, 0); CHECK(view);
        CHECK(context.pool->deactivate_entry(thread->entry_stack, {context.pid, context.pid}, 0));
    }
    if (action == Action::completed_wait)
    {
        auto wait = scheduler.register_current_wait(sched::ThreadWaitReason::deadline, 93); CHECK(wait);
        CHECK(scheduler.complete_wait(wait.value(), sched::WaitCompletion::cancelled));
        CHECK(scheduler.activate_thread(context.pid, context.pid));
    }
    if (action == Action::redispatch) CHECK(scheduler.activate_thread(context.pid, context.pid));
    if (action == Action::close_and_reuse)
    {
        auto *process = scheduler.find(context.pid); CHECK(process);
        CHECK(scheduler.close_descriptor(process->resources(), 3));
        auto opened = context.service->open(); CHECK(opened);
        context.replacement_endpoint = opened.value();
        auto allocated = scheduler.allocate_description({.used = true, .readable = true, .writable = true,
            .flags = posix::o_RDWR | posix::o_NONBLOCK, .native_udp_handle = opened.value()}); CHECK(allocated);
        CHECK(scheduler.install_descriptor(process->resources(), 3, allocated.value(), false));
    }
}
}

namespace ok::arch::execution
{
Result<Trap> enter_owned(AddressSpace &, UserFrame &, const KernelEntryStackView &view)
{
    ++backend_entries;
    auto trap = model::trap();
    trap.kernel_stack_pointer = model::context.bad_stack ? 0 : view.top() - 128;
    return trap;
}
Syscall syscall(const UserFrame &) { return {}; } // This model uses Linux NativeBridge, never native dispatch.
bool user_range(const AddressSpace &, uptr, usize, bool)
{ ++mapping_queries; return false; }
Status destroy_space(AddressSpace &space) { space = {}; return {}; }
Status copy_from_user(const AddressSpace &space, void *output, uptr address, usize bytes)
{
    ++input_copies;
    CHECK(native::BackendCapture::same_space(space, model::context.space));
    if (model::context.copy_fault || address < user_begin || address > user_begin + model::context.memory.size() ||
        bytes > user_begin + model::context.memory.size() - address) return Status::fault();
    std::memcpy(output, model::context.memory.data() + address - user_begin, bytes);
    model::after_copy();
    return {};
}
Status copy_to_user(const AddressSpace &space, uptr address, const void *input, usize bytes)
{
    ++output_copies;
    CHECK(native::BackendCapture::same_space(space, model::context.space));
    if (model::context.copy_fault || address < user_begin || address > user_begin + model::context.memory.size() ||
        bytes > user_begin + model::context.memory.size() - address) return Status::fault();
    std::memcpy(model::context.memory.data() + address - user_begin, input, bytes);
    model::after_copy();
    return {};
}
}

constexpr std::array<std::array<u32, 5>, 10> numbers{{
    {359, 361, 369, 371, 102}, {41, 49, 44, 45, lu::unavailable_number},
    {198, 200, 206, 207, lu::unavailable_number}, {281, 282, 290, 292, lu::unavailable_number},
    {198, 200, 206, 207, lu::unavailable_number}, {198, 200, 206, 207, lu::unavailable_number},
    {198, 200, 206, 207, lu::unavailable_number}, {4183, 4169, 4180, 4176, 4102},
    {5040, 5048, 5043, 5044, lu::unavailable_number}, {326, 327, 335, 337, 102},
}};
struct alignas(4096) StackBacking
{ std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> bytes{}; };

template <lu::Profile P> struct Fixture
{
    using Origins = native::SyscallOriginTable<P>;
    using Access = native::LinuxUdpImmediateAccess<P>;
    std::unique_ptr<StackBacking> backing{std::make_unique<StackBacking>()};
    memory::KernelEntryStackPool pool;
    Origins origins{pool};
    native::VfsStatContinuation<P> vfs{pool};
    native::SharedNativeCapture<P> shared{pool};
    std::unique_ptr<sched::Scheduler> scheduler{std::make_unique<sched::Scheduler>()};
    net::NativeUdpService service;
    sched::ProcessId pid{};
    u32 endpoint{};
    u16 description{};
    bool retained{};
    Fixture()
    {
        CHECK(pool.activate(backing->bytes));
        auto created = scheduler->create_user_process("linux-udp-current-image-host",
            arch::configured_arch_operations().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(created);
        pid = created.value();
        process().resources().personality = sched::UserPersonality::linux;
        process().resources().native_space = {0x100000, 1, 1};
        CHECK(native::prepare_entry_stack(pool, thread())); thread().native_active = true;
        CHECK(origins.prepare(thread())); CHECK(vfs.prepare(thread()));
        CHECK(scheduler->set_runnable(pid)); CHECK(scheduler->activate_thread(pid, pid));
        CHECK(service.configure({.local_mac = {2, 0, 0, 0, 0, 1}, .peer_mac = {2, 0, 0, 0, 0, 2},
            .local_ip = {10, 0, 2, 15}, .peer_ip = {10, 0, 2, 2}}));
        CHECK(scheduler->set_native_udp_service(service));
        auto opened = service.open(); CHECK(opened); endpoint = opened.value();
        auto allocated = scheduler->allocate_description({.used = true, .readable = true, .writable = true,
            .flags = posix::o_RDWR | posix::o_NONBLOCK, .native_udp_handle = endpoint}); CHECK(allocated); description = allocated.value();
        CHECK(scheduler->install_descriptor(process().resources(), 3, description, false));
        model::context = {.pool = &pool, .scheduler = scheduler.get(), .service = &service, .pid = pid,
            .profile = P, .space = process().resources().native_space};
    }
    ~Fixture()
    {
        origins.retire(thread());
        if (retained) CHECK(scheduler->release_description(description));
        if (process().fd_table().entry(3)->used) CHECK(scheduler->close_descriptor(process().resources(), 3));
        CHECK(vfs.cancel_process(*scheduler, pid)); CHECK(native::retire_entry_stack(pool, thread()));
        CHECK(pool.accounting().leased_slots == 0 && service.active_sockets() == 0);
        model::context = {};
    }
    sched::ProcessControlBlock &process() { return *scheduler->find(pid); }
    sched::ThreadControlBlock &thread() { return *scheduler->find_thread(pid, pid); }
    hw::UserFrame frame(u32 number)
    {
        hw::UserFrame result{}; result.pc = hw::user_begin + 256; result.sp = hw::user_begin + 4096;
        result.fp_state[23] = 83;
        if constexpr (P == lu::Profile::i386) result.registers[7] = number;
        else if constexpr (P == lu::Profile::x86_64) result.registers[14] = number;
        else if constexpr (P == lu::Profile::aarch64) result.registers[8] = number;
        else if constexpr (P == lu::Profile::arm32)
        { result.registers[7] = number; result.flags = 0x10; result.registers[15] = result.pc; }
        else if constexpr (P == lu::Profile::rv64 || P == lu::Profile::rv32) result.registers[17] = number;
        else if constexpr (P == lu::Profile::loongarch64) result.registers[11] = number;
        else if constexpr (P == lu::Profile::mips || P == lu::Profile::mips64) result.registers[2] = number;
        else result.registers[0] = number;
        return result;
    }
    Status observe(u32 number)
    {
        CHECK(scheduler->activate_thread(pid, pid));
        thread().syscall_pending = false; thread().native_frame = frame(number);
        auto view = pool.activate_entry(thread().entry_stack, {pid, pid}, 0); CHECK(view);
        const auto before = backend_entries;
        auto observed = shared.enter(origins, vfs, process(), thread(), view.value(), *scheduler);
        CHECK(backend_entries == before + 1);
        CHECK(pool.deactivate_entry(thread().entry_stack, {pid, pid}, 0));
        thread().syscall_pending = observed.ok();
        return observed ? Status{} : observed.status();
    }
    auto access() { return Access::acquire(origins, process(), thread()); }
};

template <lu::Profile P> void exercise()
{
    using F = Fixture<P>; using Access = typename F::Access; using Origins = typename F::Origins;
    using Capture = typename Origins::LinuxUdpImmediateCapture;
    using ReceiveCapture = typename Origins::LinuxUdpReceiveCapture;
    using ReceiveAdmission = native::LinuxUdpReceiveAdmission<P>;
    static_assert(!std::is_copy_constructible_v<ReceiveCapture> && !std::is_default_constructible_v<ReceiveCapture>);
    static_assert(std::is_nothrow_move_constructible_v<ReceiveCapture> && !std::is_move_assignable_v<ReceiveCapture>);
    static_assert(!std::is_copy_constructible_v<ReceiveAdmission> && !std::is_default_constructible_v<ReceiveAdmission>);
    static_assert(!std::is_constructible_v<ReceiveAdmission, lu::UdpRequest, Status>);
    static_assert(Origins::template supported_binding<typename Origins::LinuxUdpReceiveBinding>);
    static_assert(!std::same_as<typename Origins::LinuxUdpRestartPermit, typename Origins::RestartPermit>);
    static_assert(!requires(Origins &origins, const typename Origins::Identity &identity,
                           const hw::UserFrame &frame, const typename Origins::LinuxUdpReceiveBinding &binding) {
        origins.bind(identity, frame, binding);
    }); // Only the sealed receive coordinator binds this kind.
    static_assert(!std::is_default_constructible_v<Capture> && !std::is_copy_constructible_v<Capture>);
    static_assert(!std::is_constructible_v<Capture, Origins *, typename Origins::Identity>);
    static_assert(!std::is_convertible_v<Capture, typename Origins::Identity>);
    static_assert(std::is_nothrow_move_constructible_v<Capture> && !std::is_move_assignable_v<Capture>);
    static_assert(!std::is_default_constructible_v<Access> && !std::is_copy_constructible_v<Access>);
    static_assert(std::is_nothrow_move_constructible_v<Access> && !std::is_move_assignable_v<Access>);
    static_assert(!Origins::template supported_binding<Capture>);
    const auto &row = numbers[static_cast<usize>(P)];
    for (usize index = 0; index < row.size(); ++index)
    {
        if (row[index] == lu::unavailable_number) continue;
        F f; CHECK(!f.access()); CHECK(f.observe(row[index]));
        CHECK(!f.origins.live(f.origins.identity(f.process(), f.thread())));
        auto acquired = f.access(); CHECK(acquired);
        auto access = std::move(acquired.value()); CHECK(!acquired.value().live());
        CHECK(access.live() && access.raw_syscall() == row[index]);
        CHECK(access.operation() == static_cast<native::LinuxUdpImmediateOperation>(index + 1));
        CHECK(!f.access());
        auto copied = f.thread(); CHECK(!Access::acquire(f.origins, f.process(), copied));
        Origins foreign(f.pool); CHECK(!Access::acquire(foreign, f.process(), f.thread()));
        const auto queries = mapping_queries, reads = input_copies, writes = output_copies;
        CHECK(access.range(hw::user_begin + 1024, 4, false));
        CHECK(mapping_queries == queries && input_copies == reads && output_copies == writes);
        std::array<std::byte, 4> bytes{std::byte{'A'}, std::byte{'B'}, std::byte{'C'}, std::byte{'D'}};
        CHECK(access.write(hw::user_begin, bytes)); CHECK(access.read(hw::user_begin, bytes));
        CHECK(bytes[0] == std::byte{'A'} && bytes[3] == std::byte{'D'});
        CHECK(access.read(0, {}) && access.write(0, {}));
        CHECK(input_copies == reads + 1 && output_copies == writes + 1);
        CHECK(!access.range(hw::user_end - 1, 2, true));
        CHECK(!access.range(hw::user_begin - 1, 1, false));
        if constexpr (lu::Codec<P>::profile.word_bytes == 4)
            CHECK(!access.range(static_cast<uptr>(u64{1} << 32), 0, false));
        const auto old_space = f.process().resources().native_space;
        CHECK(!f.origins.stage_space(f.scheduler.get(), f.process().resources(), old_space,
            {old_space.root + 4096, old_space.slot, old_space.generation + 1}));
        CHECK(access.live());
        access.reset(); access.reset(); CHECK(!access.live() && !f.access());
    }
    for (unsigned mode = 0; mode < 5; ++mode)
    {
        F f;
        if (mode == 0) model::context.bad_stack = true;
        if (mode == 1) model::context.bad_trap = true;
        if (mode == 2) f.process().resources().personality = sched::UserPersonality::native;
        auto observed = f.observe(mode == 3 ? 0 : mode == 4 ? 0x40000000U | row[0] : row[0]);
        CHECK(mode == 0 ? !observed.ok() : observed.ok()); CHECK(!f.access());
    }
    {
        F f; CHECK(f.observe(row[1])); auto first = f.access(); CHECK(first);
        f.origins.revoke(f.thread()); CHECK(!first.value().live()); CHECK(f.observe(row[2]));
        auto second = f.access(); CHECK(second); first.value().reset(); CHECK(second.value().live());
        f.origins.revoke_process(f.pid); CHECK(!second.value().live());
    }
    for (bool writing : {false, true})
    {
        F f; CHECK(f.observe(row[3])); auto access = f.access(); CHECK(access);
        std::array<std::byte, 4> bytes{};
        model::context.action = model::Action::rotate_epoch;
        const auto status = writing ? access.value().write(hw::user_begin, bytes) : access.value().read(hw::user_begin, bytes);
        CHECK(status.code() == StatusCode::denied && !access.value().live());
        CHECK(!f.access());
    }
    {
        F f; CHECK(f.observe(row[1])); auto access = f.access(); CHECK(access);
        CHECK(f.scheduler->retain_description(f.description)); f.retained = true;
        CHECK(f.service.bind(f.endpoint, {0, 0, 0, 0}, 41000));
        model::context.action = model::Action::close_and_reuse;
        std::array<std::byte, 4> bytes{}; CHECK(access.value().read(hw::user_begin, bytes));
        CHECK(access.value().live() && model::context.replacement_endpoint != f.endpoint);
        auto original = f.scheduler->open_description(f.description); CHECK(original);
        CHECK(original->references == 1 && original->native_udp_handle == f.endpoint);
        auto binding = f.service.binding(f.endpoint); CHECK(binding && binding.value().bound && binding.value().port == 41000);
        auto replacement = f.service.binding(model::context.replacement_endpoint); CHECK(replacement && !replacement.value().bound);
        access.value().reset(); CHECK(f.scheduler->release_description(f.description)); f.retained = false;
        CHECK(!f.service.binding(f.endpoint));
    }
    {
        F f; CHECK(f.observe(row[3])); auto access = f.access(); CHECK(access);
        std::array<std::byte, 4> bytes{}; const auto reads = input_copies;
        model::context.copy_fault = true; CHECK(access.value().read(hw::user_begin, bytes).code() == StatusCode::fault);
        CHECK(access.value().live() && input_copies == reads + 1);
        model::context.copy_fault = false;
        auto wait = f.scheduler->register_current_wait(sched::ThreadWaitReason::deadline, 42); CHECK(wait);
        CHECK(!access.value().read(hw::user_begin, bytes)); CHECK(!access.value().live() && input_copies == reads + 1);
        CHECK(f.scheduler->complete_wait(wait.value(), sched::WaitCompletion::cancelled)); CHECK(f.scheduler->activate_thread(f.pid, f.pid));
    }
    // Public wait completion and redispatch must not restore the old capture.
    for (auto action : {model::Action::completed_wait, model::Action::redispatch})
        for (bool writing : {false, true})
        {
            F f; CHECK(f.observe(row[3])); auto access = f.access(); CHECK(access);
            std::array<std::byte, 4> bytes{};
            model::context.action = action;
            const auto status = writing ? access.value().write(hw::user_begin, bytes) : access.value().read(hw::user_begin, bytes);
            CHECK(status.code() == StatusCode::denied && !access.value().live() && !f.access());
        }
    {
        F f; CHECK(f.observe(row[1])); auto access = f.access(); CHECK(access);
        const auto reads = input_copies;
        auto wait = f.scheduler->register_current_wait(sched::ThreadWaitReason::deadline, 94); CHECK(wait);
        CHECK(f.scheduler->complete_wait(wait.value(), sched::WaitCompletion::cancelled));
        CHECK(f.scheduler->activate_thread(f.pid, f.pid));
        CHECK(!access.value().live() && !f.access());
        std::array<std::byte, 4> bytes{}; CHECK(!access.value().read(hw::user_begin, bytes));
        CHECK(input_copies == reads);
    }
    {
        F f; CHECK(f.observe(row[1])); auto access = f.access(); CHECK(access);
        const auto frame = f.thread().native_frame;
        ++f.thread().native_frame.fp_state[23];
        CHECK(!access.value().live());
        f.thread().native_frame = frame;
        CHECK(!access.value().live() && !f.access());
        std::array<std::byte, 4> bytes{}; const auto reads = input_copies;
        CHECK(!access.value().read(hw::user_begin, bytes) && input_copies == reads);
    }
    {
        F f; CHECK(f.observe(row[1])); auto access = f.access(); CHECK(access);
        const auto space = f.process().resources().native_space;
        ++f.process().resources().native_space.generation;
        CHECK(!access.value().live()); f.process().resources().native_space = space;
        CHECK(!access.value().live() && !f.access());
    }
    {
        F f; CHECK(f.observe(row[1])); auto access = f.access(); CHECK(access);
        CHECK(f.origins.prepare(f.thread())); CHECK(!access.value().live() && !f.access());
    }
    ++profile_cases;
}

int main()
{
    exercise<lu::Profile::i386>(); exercise<lu::Profile::x86_64>(); exercise<lu::Profile::aarch64>();
    exercise<lu::Profile::arm32>(); exercise<lu::Profile::rv64>(); exercise<lu::Profile::rv32>();
    exercise<lu::Profile::loongarch64>(); exercise<lu::Profile::mips>(); exercise<lu::Profile::mips64>(); exercise<lu::Profile::ppc>();
    CHECK(mapping_queries == 0);
    std::printf("LINUX_UDP_IMMEDIATE_ACCESS_PASS checks=%zu profiles=%zu backend_entries=%zu reads=%zu writes=%zu mapping_queries=%zu hosted=1 guest=0 nic=0 dispatcher=0\n",
        checks, profile_cases, backend_entries, input_copies, output_copies, mapping_queries);
}
