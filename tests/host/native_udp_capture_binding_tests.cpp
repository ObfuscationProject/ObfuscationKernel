// Owned backend wrapper, canonical scheduler/OFD and hosted trap/mapping models.
#include "../../src/user/native_shared_capture.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/user/native_entry_stacks.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

using namespace ok;
namespace hw = arch::execution;
namespace lu = abi::linux_user;
namespace native = user::native;
constexpr auto profile = lu::configured_native_profile();
using Origins = native::SyscallOriginTable<profile>;
using Request = Origins::NativeUdpReceiveRequest;
using Binding = Origins::NativeUdpReceiveBinding;
using WaitState = Origins::NativeUdpWaitState;
static usize checks{}, backend_entries{}, executable_copies{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "NATIVE_UDP_CAPTURE_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)
static_assert(!std::is_default_constructible_v<Origins::NativeUdpCapture>);
static_assert(!std::is_constructible_v<Origins::NativeUdpCapture, const Origins *, Origins::Identity>);
static_assert(!std::is_convertible_v<Origins::NativeUdpCapture, Origins::Identity>);
static_assert(std::is_nothrow_copy_constructible_v<Origins::NativeUdpCapture>);
static_assert(!Origins::supported_binding<Binding>);
static_assert(static_cast<u8>(sched::ThreadWaitReason::udp_receive) == 11);
static_assert(static_cast<u8>(sched::ThreadWaitReason::vfs_operation) == 10);
static_assert(sizeof(ok_sockaddr_in) == 16);

namespace model
{
bool invalid_stack{};
bool wrong_trap{};
hw::Trap trap()
{
    if (wrong_trap) return {.kind = hw::TrapKind::timer};
    if constexpr (profile == lu::Profile::x86_64) return {hw::TrapKind::syscall, 256};
    if constexpr (profile == lu::Profile::i386) return {hw::TrapKind::syscall, 128};
    if constexpr (profile == lu::Profile::aarch64) return {hw::TrapKind::syscall, 0x15, 0x56000000};
    if constexpr (profile == lu::Profile::arm32) return {hw::TrapKind::syscall, 2};
    if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64) return {hw::TrapKind::syscall, 8, 8};
    if constexpr (profile == lu::Profile::loongarch64) return {hw::TrapKind::syscall, 11};
    if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64) return {hw::TrapKind::syscall, 8, 32};
    return {hw::TrapKind::syscall, 8};
}
}
namespace ok::arch::execution
{
Result<Trap> enter_owned(AddressSpace &, UserFrame &, const KernelEntryStackView &view)
{
    ++backend_entries;
    auto trap = model::trap();
    trap.kernel_stack_pointer = model::invalid_stack ? 0 : view.top() - 128;
    return trap;
}
Syscall syscall(const UserFrame &f)
{
    if constexpr (profile == lu::Profile::x86_64)
        return {f.registers[14], {f.registers[9], f.registers[10], f.registers[11], f.registers[5], f.registers[7], f.registers[6]}};
    if constexpr (profile == lu::Profile::i386)
        return {f.registers[7], {f.registers[4], f.registers[6], f.registers[5], f.registers[1], f.registers[0], f.registers[2]}};
    if constexpr (profile == lu::Profile::aarch64 || profile == lu::Profile::arm32)
        return {f.registers[profile == lu::Profile::arm32 ? 7 : 8], {f.registers[0], f.registers[1], f.registers[2], f.registers[3], f.registers[4], f.registers[5]}};
    if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64)
        return {f.registers[17], {f.registers[10], f.registers[11], f.registers[12], f.registers[13], f.registers[14], f.registers[15]}};
    if constexpr (profile == lu::Profile::loongarch64)
        return {f.registers[11], {f.registers[4], f.registers[5], f.registers[6], f.registers[7], f.registers[8], f.registers[9]}};
    if constexpr (profile == lu::Profile::mips || profile == lu::Profile::mips64)
        return {f.registers[2], {f.registers[4], f.registers[5], f.registers[6], f.registers[7], f.registers[8], f.registers[9]}};
    return {f.registers[0], {f.registers[3], f.registers[4], f.registers[5], f.registers[6], f.registers[7], f.registers[8]}};
}
bool user_range(const AddressSpace &, uptr, usize, bool) { return false; }
Status copy_from_user(const AddressSpace &, void *, uptr, usize)
{ ++executable_copies; return Status::fault("native continuation does not copy restart opcodes"); }
Status destroy_space(AddressSpace &space) { space = {}; return {}; }
}

hw::UserFrame frame(const Request &r, uptr number = static_cast<uptr>(syscall::Number::recvfrom))
{
    hw::UserFrame f{};
    f.pc = hw::user_begin + 256; f.sp = hw::user_begin + 4096;
    f.fp_state[31] = 77;
    const std::array<uptr, 6> a{static_cast<uptr>(r.fd), r.output, r.count, r.flags, r.source_address, r.source_length};
    if constexpr (profile == lu::Profile::x86_64)
    { f.registers[14] = number; const std::array<usize, 6> slots{9, 10, 11, 5, 7, 6}; for (usize i = 0; i < 6; ++i) f.registers[slots[i]] = a[i]; }
    else if constexpr (profile == lu::Profile::i386)
    { f.registers[7] = number; const std::array<usize, 6> slots{4, 6, 5, 1, 0, 2}; for (usize i = 0; i < 6; ++i) f.registers[slots[i]] = a[i]; }
    else if constexpr (profile == lu::Profile::aarch64 || profile == lu::Profile::arm32)
    { f.registers[profile == lu::Profile::arm32 ? 7 : 8] = number; for (usize i = 0; i < 6; ++i) f.registers[i] = a[i]; }
    else if constexpr (profile == lu::Profile::rv32 || profile == lu::Profile::rv64)
    { f.registers[17] = number; for (usize i = 0; i < 6; ++i) f.registers[i + 10] = a[i]; }
    else if constexpr (profile == lu::Profile::loongarch64 || profile == lu::Profile::mips || profile == lu::Profile::mips64)
    { f.registers[profile == lu::Profile::loongarch64 ? 11 : 2] = number; for (usize i = 0; i < 6; ++i) f.registers[i + 4] = a[i]; }
    else { f.registers[0] = number; for (usize i = 0; i < 6; ++i) f.registers[i + 3] = a[i]; }
    return f;
}
struct alignas(4096) StackBacking { std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> bytes{}; };
struct ObjectPages
{
    static constexpr usize page_size = 4096;
    struct Lease { std::span<std::byte> bytes{}; };
    alignas(4096) std::array<std::byte, page_size> bytes{};
    bool used{};
    Result<Lease> allocate_kernel_page()
    { if (used) return Status::no_memory(); used = true; return Lease{bytes}; }
    Status release_kernel_page(Lease lease)
    { if (!used || lease.bytes.data() != bytes.data()) return Status::denied(); used = false; return {}; }
};
struct Fixture
{
    std::unique_ptr<StackBacking> backing{std::make_unique<StackBacking>()};
    memory::KernelEntryStackPool pool;
    Origins origins{pool};
    native::VfsStatContinuation<profile> vfs{pool};
    native::SharedNativeCapture<profile> shared{pool};
    std::unique_ptr<sched::Scheduler> scheduler{std::make_unique<sched::Scheduler>()};
    net::NativeUdpService service;
    ObjectPages pages;
    memory::ObjectPool<u64, ObjectPages, 1> objects{pages};
    memory::ObjectHandle object{};
    sched::ProcessId pid{};
    u16 description{};
    u32 endpoint{};
    bool retained{};
    Request request{.fd = 3, .output = hw::user_begin + 512, .count = 0};
    Fixture()
    {
        CHECK(pool.activate(backing->bytes));
        auto made = scheduler->create_user_process("udp-capture-host-model",
            arch::configured_arch_operations().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(made);
        pid = made.value();
        process().resources().personality = sched::UserPersonality::native;
        process().resources().native_space = {0x100000, 1, 1};
        CHECK(native::prepare_entry_stack(pool, thread())); thread().native_active = true;
        CHECK(origins.prepare(thread())); CHECK(vfs.prepare(thread()));
        CHECK(scheduler->set_runnable(pid)); CHECK(scheduler->activate_thread(pid, pid));
        CHECK(service.configure({.local_mac = {2, 0, 0, 0, 0, 1}, .peer_mac = {2, 0, 0, 0, 0, 2},
                                 .local_ip = {10, 0, 2, 15}, .peer_ip = {10, 0, 2, 2}}));
        CHECK(scheduler->set_native_udp_service(service));
        auto opened = service.open(); CHECK(opened); endpoint = opened.value();
        auto allocated = scheduler->allocate_description({.used = true, .readable = true, .writable = true, .native_udp_handle = endpoint}); CHECK(allocated);
        description = allocated.value(); CHECK(scheduler->install_descriptor(process().resources(), request.fd, description, false));
        auto made_object = objects.create(); CHECK(made_object); object = made_object.value();
    }
    ~Fixture()
    {
        origins.retire(thread());
        if (thread().wait_registration.active()) CHECK(scheduler->complete_wait(thread().wait_registration.ticket(), sched::WaitCompletion::cancelled));
        if (retained) CHECK(scheduler->release_description(description));
        if (process().fd_table().entry(3)->used) CHECK(scheduler->close_descriptor(process().resources(), 3));
        CHECK(objects.destroy(object)); CHECK(objects.trim_empty_pages());
        CHECK(vfs.cancel_process(*scheduler, pid)); CHECK(native::retire_entry_stack(pool, thread()));
    }
    sched::ProcessControlBlock &process() { return *scheduler->find(pid); }
    sched::ThreadControlBlock &thread() { return *scheduler->find_thread(pid, pid); }
    Status observe(uptr number = static_cast<uptr>(syscall::Number::recvfrom))
    {
        CHECK(scheduler->activate_thread(pid, pid)); thread().syscall_pending = false; thread().native_frame = frame(request, number);
        auto view = pool.activate_entry(thread().entry_stack, {pid, pid}, 0); CHECK(view);
        const auto before = backend_entries;
        auto observed = shared.enter(origins, vfs, process(), thread(), view.value(), *scheduler);
        CHECK(backend_entries == before + 1);
        CHECK(pool.deactivate_entry(thread().entry_stack, {pid, pid}, 0));
        thread().syscall_pending = observed.ok();
        return observed ? Status{} : observed.status();
    }
    Binding arm()
    {
        if (!retained) { CHECK(scheduler->retain_description(description)); retained = true; }
        auto ticket = scheduler->register_current_wait(sched::ThreadWaitReason::udp_receive, endpoint); CHECK(ticket);
        return {object, ticket.value(), ticket.value().generation, description, endpoint, request};
    }
};

void binding_and_rearm()
{
    Fixture f; f.request.source_address = hw::user_begin + 1024; f.request.source_length = hw::user_begin + 2048;
    f.request.source_capacity = 32; CHECK(f.observe());
    auto captured = f.origins.native_udp_capture(f.process(), f.thread()); CHECK(captured);
    CHECK(captured.value().sequence() && captured.value().pid() == f.pid && captured.value().tid() == f.pid);
    CHECK(!f.origins.identity(f.process(), f.thread()).capture);
    CHECK(!f.origins.live(f.origins.identity(f.process(), f.thread())));
    auto binding = f.arm();
    for (unsigned change = 0; change < 15; ++change)
    {
        auto wrong = binding;
        switch (change)
        {
        case 0: wrong.object.pool = nullptr; break;
        case 1: wrong.object.generation = 0; break;
        case 2: ++wrong.ticket.generation; break;
        case 3: ++wrong.ticket.owner; break;
        case 4: ++wrong.ticket.thread; break;
        case 5: ++wrong.request; break;
        case 6: wrong.description = sched::invalid_description; break;
        case 7: ++wrong.endpoint; break;
        case 8: ++wrong.operation.fd; break;
        case 9: ++wrong.operation.output; break;
        case 10: ++wrong.operation.count; break;
        case 11: wrong.operation.flags = 1; break;
        case 12: ++wrong.operation.source_address; break;
        case 13: ++wrong.operation.source_length; break;
        case 14: wrong.operation.source_capacity = 15; break;
        }
        CHECK(!f.origins.bind_native_udp(captured.value(), f.thread().native_frame, wrong));
    }
    auto changed_frame = f.thread().native_frame; ++changed_frame.fp_state[31];
    CHECK(!f.origins.bind_native_udp(captured.value(), changed_frame, binding));
    CHECK(f.origins.bind_native_udp(captured.value(), f.thread().native_frame, binding));
    CHECK(!f.origins.bind_native_udp(captured.value(), f.thread().native_frame, binding));
    CHECK(f.origins.validate_native_udp(captured.value(), f.thread().native_frame, binding, WaitState::pending));
    CHECK(!f.origins.validate_native_udp(captured.value(), f.thread().native_frame, binding, WaitState::ready));
    CHECK(!f.origins.native_udp_capture(f.process(), f.thread()));
    CHECK(!f.origins.prepare_restart(Origins::Identity{}, f.thread().native_frame, Origins::PipeBinding{}));
    const auto old_space = f.process().resources().native_space;
    const hw::AddressSpace new_space{old_space.root + 4096, old_space.slot, old_space.generation + 1};
    auto update = f.origins.stage_space(f.scheduler.get(), f.process().resources(), old_space, new_space); CHECK(update);
    f.process().resources().native_space = new_space; CHECK(f.origins.commit_space(update.value(), f.scheduler.get(), f.process().resources()));
    CHECK(f.origins.validate_native_udp(captured.value(), f.thread().native_frame, binding, WaitState::pending));
    CHECK(f.scheduler->complete_wait(binding.ticket, sched::WaitCompletion::ready));
    CHECK(f.origins.validate_native_udp(captured.value(), f.thread().native_frame, binding, WaitState::ready));
    CHECK(f.scheduler->activate_thread(f.pid, f.pid));
    auto rearmed = f.scheduler->register_current_wait(sched::ThreadWaitReason::udp_receive, f.endpoint); CHECK(rearmed);
    auto after = binding; after.ticket = rearmed.value();
    auto altered = after; ++altered.operation.source_capacity;
    CHECK(!f.origins.rearm_native_udp(captured.value(), f.thread().native_frame, binding, altered));
    CHECK(f.origins.rearm_native_udp(captured.value(), f.thread().native_frame, binding, after));
    CHECK(!f.scheduler->complete_wait(binding.ticket, sched::WaitCompletion::ready));
    f.origins.retire_native_udp(captured.value(), binding);
    CHECK(f.origins.validate_native_udp(captured.value(), f.thread().native_frame, after, WaitState::pending));
    CHECK(f.scheduler->close_descriptor(f.process().resources(), f.request.fd));
    CHECK(f.service.active_sockets() == 1);
    CHECK(f.origins.validate_native_udp(captured.value(), f.thread().native_frame, after, WaitState::pending));
    CHECK(f.scheduler->complete_wait(after.ticket, sched::WaitCompletion::interrupted));
    CHECK(f.origins.validate_native_udp(captured.value(), f.thread().native_frame, after, WaitState::interrupted));
    CHECK(!f.origins.validate_native_udp(captured.value(), f.thread().native_frame, after, WaitState::ready));
    f.origins.retire_native_udp(captured.value(), after); f.origins.retire_native_udp(captured.value(), after);
    CHECK(!f.origins.validate_native_udp(captured.value(), f.thread().native_frame, after, WaitState::interrupted));
}

void retirement_and_replacement()
{
    Fixture f; CHECK(f.observe()); auto token = f.origins.native_udp_capture(f.process(), f.thread()); CHECK(token);
    auto binding = f.arm(); CHECK(f.origins.bind_native_udp(token.value(), f.thread().native_frame, binding));
    CHECK(f.scheduler->complete_wait(binding.ticket, sched::WaitCompletion::cancelled));
    CHECK(f.origins.validate_native_udp(token.value(), f.thread().native_frame, binding, WaitState::cancelled));
    CHECK(f.observe()); auto next = f.origins.native_udp_capture(f.process(), f.thread()); CHECK(next);
    CHECK(next.value().sequence() > token.value().sequence());
    auto replacement = f.arm(); CHECK(f.origins.bind_native_udp(next.value(), f.thread().native_frame, replacement));
    f.origins.retire_native_udp(token.value(), binding);
    CHECK(f.origins.validate_native_udp(next.value(), f.thread().native_frame, replacement, WaitState::pending));
    auto modified = f.thread().native_frame; ++modified.fp_state[31];
    CHECK(!f.origins.validate_native_udp(next.value(), modified, replacement, WaitState::pending));
    f.thread().native_frame = modified;
    f.origins.retire_native_udp(next.value(), replacement);
    CHECK(!f.origins.validate_native_udp(next.value(), modified, replacement, WaitState::pending));
}

void observation_guards()
{
    for (unsigned mode = 0; mode < 6; ++mode)
    {
        Fixture f;
        if (mode == 0) f.request.flags = 1;
        if (mode == 1) f.request.source_address = hw::user_begin + 1024;
        if (mode == 2) f.request.fd = -1;
        if (mode == 3) model::wrong_trap = true;
        if (mode == 4) model::invalid_stack = true;
        auto observed = f.observe(mode == 5 ? static_cast<uptr>(syscall::Number::sendto) : static_cast<uptr>(syscall::Number::recvfrom));
        CHECK(mode == 4 ? !observed.ok() : observed.ok());
        CHECK(!f.origins.native_udp_capture(f.process(), f.thread()));
        model::wrong_trap = model::invalid_stack = false;
    }
    Fixture f; CHECK(f.observe()); auto token = f.origins.native_udp_capture(f.process(), f.thread()); CHECK(token);
    Origins foreign(f.pool); auto binding = f.arm();
    CHECK(!foreign.bind_native_udp(token.value(), f.thread().native_frame, binding));
    CHECK(f.origins.bind_native_udp(token.value(), f.thread().native_frame, binding));
    foreign.retire_native_udp(token.value(), binding);
    CHECK(f.origins.validate_native_udp(token.value(), f.thread().native_frame, binding, WaitState::pending));
    const auto current = f.process().resources().native_space;
    f.process().resources().native_space.generation += 1;
    CHECK(!f.origins.validate_native_udp(token.value(), f.thread().native_frame, binding, WaitState::pending));
    f.process().resources().native_space = current;
    CHECK(f.origins.validate_native_udp(token.value(), f.thread().native_frame, binding, WaitState::pending));
    f.origins.revoke_process(f.pid);
    CHECK(!f.origins.validate_native_udp(token.value(), f.thread().native_frame, binding, WaitState::pending));
}
int main()
{
    binding_and_rearm(); retirement_and_replacement(); observation_guards();
    CHECK(executable_copies == 0);
    std::printf("NATIVE_UDP_CAPTURE_MODEL_PASS checks=%zu backend_entries=%zu opcode_copies=%zu hosted=1 guest=0 runtime_receive=0\n",
                checks, backend_entries, executable_copies);
}
