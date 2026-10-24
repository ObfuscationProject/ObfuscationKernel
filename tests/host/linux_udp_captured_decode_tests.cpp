// Real shared capture, origin and scheduler code; fixed synthetic trap/usercopy.
#include "../../src/user/linux_udp_captured_decode.hpp"
#include "../../src/user/linux_udp_immediate_access.hpp"
#include "../../src/user/native_shared_capture.hpp"
#include "ok/user/native_entry_stacks.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace ok;
namespace hw = arch::execution;
namespace lu = abi::linux_user;
namespace native = user::native;
static usize checks{}, profiles{}, backend_entries{}, reads{}, writes{}, mapping_queries{}, copied_bytes{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "CAPTURED_UDP_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)

namespace model
{
enum class Action : u8 { none, completed_wait, redispatch, change_frame, overwrite_memory };
struct Context
{
    sched::Scheduler *scheduler{};
    sched::ProcessId pid{};
    lu::Profile profile{};
    hw::AddressSpace space{};
    Action action{Action::none};
    bool copy_fault{};
    usize partial{};
    uptr last_address{};
    usize last_size{};
    std::array<std::byte, 256> memory{};
} context;
hw::Trap trap()
{
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
    if (action == Action::completed_wait)
    {
        auto ticket = scheduler.register_current_wait(sched::ThreadWaitReason::deadline, 83); CHECK(ticket);
        CHECK(scheduler.complete_wait(ticket.value(), sched::WaitCompletion::cancelled));
        CHECK(scheduler.activate_thread(context.pid, context.pid));
    }
    if (action == Action::redispatch) CHECK(scheduler.activate_thread(context.pid, context.pid));
    if (action == Action::change_frame)
    {
        auto *thread = scheduler.find_thread(context.pid, context.pid); CHECK(thread);
        ++thread->native_frame.registers[4];
        thread->native_frame.sp = hw::user_begin + 200;
        ++thread->native_frame.fp_state[23];
    }
    if (action == Action::overwrite_memory) context.memory.fill(std::byte{0});
}
}

namespace ok::arch::execution
{
Result<Trap> enter_owned(AddressSpace &, UserFrame &, const KernelEntryStackView &view)
{
    ++backend_entries;
    auto trap = model::trap(); trap.kernel_stack_pointer = view.top() - 128; return trap;
}
Syscall syscall(const UserFrame &) { return {}; }
bool user_range(const AddressSpace &, uptr, usize, bool) { ++mapping_queries; return false; }
Status destroy_space(AddressSpace &space) { space = {}; return {}; }
Status copy_from_user(const AddressSpace &space, void *output, uptr address, usize bytes)
{
    ++reads; model::context.last_address = address; model::context.last_size = bytes;
    CHECK(native::BackendCapture::same_space(space, model::context.space));
    const bool in_buffer = address >= user_begin && address <= user_begin + model::context.memory.size() &&
        bytes <= user_begin + model::context.memory.size() - address;
    if (!in_buffer) return Status::fault();
    const usize copied = model::context.copy_fault ? std::min(bytes, model::context.partial) : bytes;
    std::memcpy(output, model::context.memory.data() + address - user_begin, copied); copied_bytes += copied;
    model::after_copy();
    return model::context.copy_fault ? Status::fault() : Status{};
}
Status copy_to_user(const AddressSpace &, uptr, const void *, usize) { ++writes; return Status::fault(); }
}

struct alignas(4096) StackBacking
{ std::array<std::byte, memory::KernelEntryStackPool::arena_bytes> bytes{}; };

template <lu::Profile P> struct Fixture
{
    using Origins = native::SyscallOriginTable<P>;
    using Access = native::LinuxUdpImmediateAccess<P>;
    using Codec = lu::Codec<P>;
    std::unique_ptr<StackBacking> backing{std::make_unique<StackBacking>()};
    memory::KernelEntryStackPool pool;
    Origins origins{pool};
    native::VfsStatContinuation<P> vfs{pool};
    native::SharedNativeCapture<P> shared{pool};
    std::unique_ptr<sched::Scheduler> scheduler{std::make_unique<sched::Scheduler>()};
    sched::ProcessId pid{};
    Fixture()
    {
        CHECK(pool.activate(backing->bytes));
        auto created = scheduler->create_user_process("linux-udp-captured-decode-host",
            arch::configured_arch_operations().make_user_context({hw::user_begin, hw::user_begin + 4096})); CHECK(created);
        pid = created.value();
        process().resources().personality = sched::UserPersonality::linux;
        process().resources().native_space = {0x100000, 1, 1};
        CHECK(native::prepare_entry_stack(pool, thread())); thread().native_active = true;
        CHECK(origins.prepare(thread())); CHECK(vfs.prepare(thread()));
        CHECK(scheduler->set_runnable(pid)); CHECK(scheduler->activate_thread(pid, pid));
        model::context = {.scheduler = scheduler.get(), .pid = pid, .profile = P, .space = process().resources().native_space};
        model::context.memory.fill(std::byte{0xa7});
    }
    ~Fixture()
    {
        origins.retire(thread()); CHECK(vfs.cancel_process(*scheduler, pid));
        CHECK(native::retire_entry_stack(pool, thread())); CHECK(pool.accounting().leased_slots == 0);
        model::context = {};
    }
    sched::ProcessControlBlock &process() { return *scheduler->find(pid); }
    sched::ThreadControlBlock &thread() { return *scheduler->find_thread(pid, pid); }
    static void set_register(hw::UserFrame &frame, usize index, u64 value)
    {
        if constexpr (P == lu::Profile::i386)
        {
            constexpr usize slots[]{7, 6, 5, 4, 0, 2, 1, 0};
            CHECK(index < 8 && index != 4); frame.registers[slots[index]] = static_cast<uptr>(value);
        }
        else if constexpr (P == lu::Profile::x86_64)
        {
            constexpr usize slots[]{14, 11, 12, 13, 10, 9, 8, 0, 7, 6, 5, 4, 3, 2, 1, 0};
            CHECK(index < 16 && index != 7); frame.registers[slots[index]] = static_cast<uptr>(value);
        }
        else frame.registers[index] = static_cast<uptr>(value);
    }
    void word(usize offset, u64 value)
    {
        CHECK(offset + Codec::profile.word_bytes <= model::context.memory.size());
        for (usize at = 0; at < Codec::profile.word_bytes; ++at)
            model::context.memory[offset + at] = std::byte(value >> (Codec::profile.big_endian ?
                (Codec::profile.word_bytes - at - 1) * 8 : at * 8));
    }
    void observe(u32 number, std::array<u64, 6> arguments, uptr stack = hw::user_begin + 64)
    {
        CHECK(scheduler->activate_thread(pid, pid));
        auto &frame = thread().native_frame; frame = {}; frame.pc = hw::user_begin + 256; frame.sp = stack;
        frame.fp_state[23] = 73;
        if constexpr (P == lu::Profile::arm32) { frame.flags = 0x10; frame.registers[15] = frame.pc; }
        set_register(frame, Codec::profile.number_register, number);
        for (usize index = 0; index < arguments.size(); ++index)
            if (Codec::profile.argument_registers[index] >= 0)
                set_register(frame, static_cast<usize>(Codec::profile.argument_registers[index]), arguments[index]);
        thread().syscall_pending = false;
        auto view = pool.activate_entry(thread().entry_stack, {pid, pid}, 0); CHECK(view);
        const auto before = backend_entries;
        auto observed = shared.enter(origins, vfs, process(), thread(), view.value(), *scheduler); CHECK(observed);
        CHECK(backend_entries == before + 1); CHECK(pool.deactivate_entry(thread().entry_stack, {pid, pid}, 0));
        thread().syscall_pending = true;
    }
    auto acquire() { return Access::acquire(origins, process(), thread()); }
};

template <lu::Profile P> std::array<u64, 6> arguments(lu::UdpCall call)
{
    constexpr auto mask = lu::Codec<P>::word_mask;
    if (call == lu::UdpCall::socket) return {0x80000002U, 0x80080000U | lu::UdpConstants<P>::sock_dgram, 17, 0, 0, 0};
    if (call == lu::UdpCall::bind) return {0xfffffffbU, 0x1122334455667788ULL & mask, 0xfffffff0U, 0, 0, 0};
    return {0xfffffffdU, 0x23456789abcdef01ULL & mask, 0x8877665544332211ULL & mask,
        0x10204060U, 0xfedcba9876543210ULL & mask, 0x1234567889abcdefULL & mask};
}
template <lu::Profile P> lu::UdpRequest expected(lu::UdpCall call)
{
    constexpr auto mask = lu::Codec<P>::word_mask;
    lu::UdpRequest result{.call = call};
    if (call == lu::UdpCall::socket) { result.domain = std::bit_cast<i32>(u32{0x80000002}); result.type = std::bit_cast<i32>(u32{0x80080000U | lu::UdpConstants<P>::sock_dgram}); result.protocol = 17; }
    else if (call == lu::UdpCall::bind) { result.fd = -5; result.address = 0x1122334455667788ULL & mask; result.address_length = -16; }
    else
    {
        result.fd = -3; result.payload = 0x23456789abcdef01ULL & mask; result.count = 0x8877665544332211ULL & mask;
        result.flags = 0x10204060U; result.address = 0xfedcba9876543210ULL & mask;
        if (call == lu::UdpCall::sendto) result.address_length = std::bit_cast<i32>(u32{0x89abcdef});
        else result.source_length = 0x1234567889abcdefULL & mask;
    }
    return result;
}

template <lu::Profile P> void copied_cases(bool socketcall, lu::UdpCall call)
{
    using F = Fixture<P>; using Constants = lu::UdpConstants<P>;
    constexpr usize selectors[]{1, 2, 11, 12};
    const usize index = static_cast<usize>(call), words = index < 2 ? 3 : 6;
    const auto values = arguments<P>(call);
    for (auto action : {model::Action::none, model::Action::completed_wait, model::Action::redispatch,
                        model::Action::change_frame, model::Action::overwrite_memory})
        for (bool fault : {false, true})
        {
            F f;
            if (socketcall)
            {
                for (usize at = 0; at < words; ++at) f.word(128 + at * lu::Codec<P>::profile.word_bytes, values[at]);
                f.observe(*Constants::socketcall_number(), {selectors[index], hw::user_begin + 128, 0, 0, 0, 0});
            }
            else
            {
                f.word(80, values[4]); f.word(84, values[5]); f.observe(*Constants::syscall_number(call), values);
            }
            auto access = f.acquire(); CHECK(access);
            const usize before = reads, before_bytes = copied_bytes;
            model::context.copy_fault = fault; model::context.partial = 4; model::context.action = action;
            auto request = native::linux_udp::decode_captured<P>(f.thread().native_frame, access.value());
            CHECK(reads == before + 1);
            CHECK(model::context.last_address == hw::user_begin + (socketcall ? 128 : 80));
            const usize size = socketcall ? words * lu::Codec<P>::profile.word_bytes : 8;
            CHECK(model::context.last_size == size && copied_bytes == before_bytes + (fault ? 4 : size));
            const bool revoked = action == model::Action::completed_wait || action == model::Action::redispatch || action == model::Action::change_frame;
            if (revoked)
            {
                CHECK(!request && request.status().code() == StatusCode::denied && !access.value().live());
                const auto copied_before = reads;
                CHECK(!native::linux_udp::decode_captured<P>(f.thread().native_frame, access.value()));
                CHECK(reads == copied_before && !f.acquire());
            }
            else if (fault) CHECK(!request && request.status().code() == StatusCode::fault && access.value().live());
            else CHECK(request && request.value() == expected<P>(call) && access.value().live());
        }
}

template <lu::Profile P> void exercise()
{
    using F = Fixture<P>; using Constants = lu::UdpConstants<P>;
    static_assert(native::linux_udp::CapturedArgumentAccess<typename F::Access>);
    // SOCK_STREAM/DGRAM differ on MIPS; classification is target-specific and copy-free.
    {
        F f;
        auto stream_arguments = arguments<P>(lu::UdpCall::socket);
        stream_arguments[1] = (stream_arguments[1] & ~u64{Constants::sock_type_mask}) | Constants::sock_stream;
        const auto before = reads;
        f.observe(*Constants::syscall_number(lu::UdpCall::socket), stream_arguments);
        CHECK(!f.acquire() && f.origins.has_tcp_immediate(f.thread()) && reads == before);
    }
    for (usize index = 0; index < 4; ++index)
    {
        const auto call = static_cast<lu::UdpCall>(index);
        if constexpr (P == lu::Profile::mips)
            if (index >= 2) { copied_cases<P>(false, call); continue; }
        F f; f.observe(*Constants::syscall_number(call), arguments<P>(call));
        auto access = f.acquire(); CHECK(access); const auto before = reads;
        model::context.copy_fault = true;
        auto request = native::linux_udp::decode_captured<P>(f.thread().native_frame, access.value());
        CHECK(request && request.value() == expected<P>(call) && reads == before && access.value().live());
    }
    for (bool wait : {false, true})
    {
        F f; f.observe(*Constants::syscall_number(lu::UdpCall::socket), arguments<P>(lu::UdpCall::socket));
        auto access = f.acquire(); CHECK(access);
        if (wait)
        {
            auto ticket = f.scheduler->register_current_wait(sched::ThreadWaitReason::deadline, 84); CHECK(ticket);
            CHECK(f.scheduler->complete_wait(ticket.value(), sched::WaitCompletion::cancelled));
        }
        CHECK(f.scheduler->activate_thread(f.pid, f.pid)); const auto before = reads;
        auto request = native::linux_udp::decode_captured<P>(f.thread().native_frame, access.value());
        CHECK(!request && request.status().code() == StatusCode::denied && reads == before && !access.value().live());
    }
    {
        F f; f.observe(*Constants::syscall_number(lu::UdpCall::socket), arguments<P>(lu::UdpCall::socket));
        auto access = f.acquire(); CHECK(access); auto other = f.thread().native_frame;
        F::set_register(other, lu::Codec<P>::profile.number_register, *Constants::syscall_number(lu::UdpCall::bind));
        const auto before = reads; auto request = native::linux_udp::decode_captured<P>(other, access.value());
        CHECK(!request && request.status().code() == StatusCode::denied && reads == before && access.value().live());
    }
    if constexpr (P == lu::Profile::mips)
    {
        F f; f.observe(*Constants::syscall_number(lu::UdpCall::sendto), arguments<P>(lu::UdpCall::sendto), 0xffffffecU);
        auto access = f.acquire(); CHECK(access); const auto before = reads;
        auto request = native::linux_udp::decode_captured<P>(f.thread().native_frame, access.value());
        CHECK(!request && request.status().code() == StatusCode::fault && reads == before);
    }
    if (Constants::socketcall_number())
    {
        constexpr usize selectors[]{1, 2, 11, 12};
        for (usize index = 0; index < 4; ++index) copied_cases<P>(true, static_cast<lu::UdpCall>(index));
        for (u64 selector : {u64{0}, u64{3}, u64{4}, u64{5}, u64{6}, u64{7}, u64{8}, u64{9}, u64{10},
                            u64{13}, u64{14}, u64{15}, u64{16}, u64{17}, u64{18}, u64{19}, u64{20}, u64{21}, u64{0x80000001U}})
        {
            F f; const auto before = reads;
            f.observe(*Constants::socketcall_number(), {selector, hw::user_begin + 128, 0, 0, 0, 0});
            auto access = f.acquire();
            if (selector == 8 || selector == 16 || selector == 17)
            {
                // UNIX pair/message selectors never acquire UDP authority or copy user memory.
                CHECK(!access && reads == before);
                continue;
            }
            CHECK(access);
            auto request = native::linux_udp::decode_captured<P>(f.thread().native_frame, access.value());
            if (selector == 3 || selector == 6 || selector == 7)
            {
                // Three-word association/name calls preserve the copied-array contract.
                const auto expected = selector == 3 ? lu::UdpCall::connect :
                                      selector == 6 ? lu::UdpCall::getsockname : lu::UdpCall::getpeername;
                CHECK(request && request.value().call == expected &&
                      reads == before + 1 && access.value().live());
                CHECK(model::context.last_address == hw::user_begin + 128 &&
                      model::context.last_size == 3 * lu::Codec<P>::profile.word_bytes);
            }
            else
            {
                CHECK(!request && reads == before && access.value().live());
                CHECK(request.status().code() == (selector == 0 || selector > 20 ? StatusCode::invalid_argument : StatusCode::unsupported));
            }
        }
        for (usize index = 0; index < 4; ++index)
        {
            F f; f.observe(*Constants::socketcall_number(), {selectors[index], lu::Codec<P>::word_mask - 3, 0, 0, 0, 0});
            auto access = f.acquire(); CHECK(access); const auto before = reads;
            auto request = native::linux_udp::decode_captured<P>(f.thread().native_frame, access.value());
            CHECK(!request && request.status().code() == StatusCode::fault && reads == before);
        }
    }
    ++profiles;
}

int main()
{
    exercise<lu::Profile::i386>(); exercise<lu::Profile::x86_64>(); exercise<lu::Profile::aarch64>();
    exercise<lu::Profile::arm32>(); exercise<lu::Profile::rv64>(); exercise<lu::Profile::rv32>();
    exercise<lu::Profile::loongarch64>(); exercise<lu::Profile::mips>(); exercise<lu::Profile::mips64>(); exercise<lu::Profile::ppc>();
    CHECK(mapping_queries == 0 && writes == 0 && profiles == 10);
    std::printf("LINUX_UDP_CAPTURED_DECODE_PASS checks=%zu profiles=%zu backend_entries=%zu reads=%zu copied_bytes=%zu mapping_queries=%zu writes=%zu hosted=1 guest=0 dispatcher=0 nic=0\n",
        checks, profiles, backend_entries, reads, copied_bytes, mapping_queries, writes);
}
