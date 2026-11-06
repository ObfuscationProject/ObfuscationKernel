#include "ok/arch/execution.hpp"
#include "ok/arch/smp.hpp"
#include "ok/boot/boot_memory.hpp"
#include "ok/boot/cpu_topology.hpp"
#include "ok/boot/fdt.hpp"
#include "ok/boot/raw_handoff.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/smp/bringup.hpp"
#include "execution_table_pages.hpp"
#include "../../src/arch/tlb_tags.hpp"

#include <atomic>

using namespace ok;
namespace native = ok::arch::execution;

extern "C"
{
    extern const u8 ok_native_user_begin[], ok_native_user_end[];
    extern const u8 ok_native_user_kernel_read[], ok_native_user_readonly_write[];
    extern const u8 ok_native_user_nx[], ok_native_user_unmapped[], ok_native_user_invalid[];
}
namespace
{
alignas(4096) u8 code[4096]{};
alignas(4096) u8 data_a[4096]{};
alignas(4096) u8 data_b[4096]{};
alignas(4096) u8 stack_a[4096]{};
alignas(4096) u8 stack_b[4096]{};
constexpr uptr code_va = native::user_begin;
constexpr uptr data_va = native::user_begin + 8192;
constexpr uptr stack_va = native::user_end - 4096;

void print(std::string_view text)
{
    for (const auto value : text)
    {
        while ((*reinterpret_cast<volatile u32 *>(0x09000018) & 0x20) != 0)
        {
        }
        *reinterpret_cast<volatile u32 *>(0x09000000) = static_cast<u8>(value);
    }
}
[[noreturn]] void finish(bool pass)
{
    const u64 parameters[] = {0x20026, pass ? u64{0} : u64{1}};
    register u64 operation asm("x0") = 0x20;
    register const u64 *argument asm("x1") = parameters;
    asm volatile("hlt #0xf000" : "+r"(operation) : "r"(argument) : "memory");
    for (;;)
        asm volatile("msr daifset, #0xf; wfe");
}
void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        print("OK_NATIVE_TEST_FAIL ");
        print(message);
        print("\n");
        finish(false);
    }
}
native::Trap next_non_timer(native::AddressSpace &space, native::UserFrame &frame)
{
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        const auto result = native::enter(space, frame);
        require(result.ok(), "enter returned an error");
        if (result.value().kind != native::TrapKind::timer)
            return result.value();
    }
    require(false, "no forward progress after timer preemption");
    return {};
}
// One run of the user program's two syscalls; it stores `value` at data_va.
void store_through_user(native::AddressSpace &space, u64 value)
{
    auto frame = native::initial_frame(code_va, native::user_end);
    auto trap = next_non_timer(space, frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame).number == 0x1234, "tag churn entry");
    native::set_return_value(frame, value);
    trap = next_non_timer(space, frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame).number == 0x2345, "tag churn store");
}

// More spaces than this CPU has ASID slots, created and retired in turn like
// mmap churn: tags roll over and are reused, each space stores into its own
// frame (three frames rotate), and the next entry after a remap stores into
// the new frame. This runs the tagged entry path, its tlbi aside1 encoding and
// the rollover; it cannot catch a missing invalidation under QEMU TCG, which
// drops its whole TLB whenever TTBR0's ASID changes. Stale-entry detection is
// the hosted model's (scripts/test_tlb_tags.py).
alignas(4096) u8 churn_data[3][4096]{};
alignas(4096) u8 churn_stack[4096]{};
alignas(4096) u8 remap_target[4096]{};
void check_tag_churn(native::AddressSpace &kept, const u8 *kept_data)
{
    constexpr usize spaces = 2 * ok::arch::detail::tlb_tag_slots + 5;
    const auto before = native::tlb_tag_report(1);
    for (usize i = 0; i < spaces; ++i)
    {
        auto *data = churn_data[i % 3];
        const native::PageMapping mappings[] = {
            {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
            {data_va, reinterpret_cast<uptr>(data), native::page_read | native::page_write},
            {stack_va, reinterpret_cast<uptr>(churn_stack), native::page_read | native::page_write},
        };
        auto created = native::create_space(mappings);
        require(created.ok(), "create tag churn space");
        auto space = created.value();
        store_through_user(space, 0x100 + i);
        require(data[0] == static_cast<u8>(i) && data[1] == 1, "tag churn space stores into its own frame");
        require(native::destroy_space(space).ok(), "retire tag churn space");
        if (i % 7 == 0)
        {
            store_through_user(kept, 0x40 + i);
            require(kept_data[0] == static_cast<u8>(0x40 + i), "long-lived space keeps its frame across rollovers");
        }
    }
    store_through_user(kept, 0x11);
    require(kept_data[0] == 0x11, "long-lived space before remap");
    const auto before_remap = native::tlb_tag_report(1);
    require(native::remap(kept, {data_va, reinterpret_cast<uptr>(remap_target), native::page_read | native::page_write})
                .ok(),
            "remap long-lived data page");
    store_through_user(kept, 0x22);
    require(remap_target[0] == 0x22 && kept_data[0] == 0x11, "next entry after a remap uses the new frame");
    const auto after = native::tlb_tag_report(1);
    // Of the two entries after the remap, the first invalidates the space's
    // ASID (its generation moved) and the second keeps it.
    require(after.invalidations == before_remap.invalidations + 1, "one ASID invalidation for the remap");
    require(after.supported && after.asid_bits >= 8 && !after.pcid && after.rollovers >= before.rollovers + 2,
            "ASID tags rolled over");
    if (after.asid_bits == 16)
        print("OK_NATIVE_TLB scope=hardware asid=16");
    else
        print("OK_NATIVE_TLB scope=hardware asid=8");
    print(" churn=pass rollover=pass remap=pass\n");
}

u64 counter()
{
    u64 value;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(value));
    return value;
}
// Bounded by the architectural counter, so a CPU that never answers fails.
template <typename Done> bool wait_for(Done done, u64 milliseconds)
{
    u64 frequency;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    const auto start = counter(), limit = frequency / 1000 * milliseconds;
    while (!done())
    {
        if (counter() - start > limit)
            return done();
        ok::arch::ArchOperations::spin_pause();
    }
    return true;
}

// A secondary CPU started through PSCI CPU_ON runs EL0 code in its own space
// on an owned entry stack while the boot CPU runs in another. `secondary_lock`
// stands in for the kernel lock: a CPU opens its user window under it, and a
// translation change holds it. After the boot CPU remapped the secondary's
// data page (a shootdown), the secondary enters that space again and must
// store into the new frame: the lazy path of a CPU that cached the old
// translation under its own ASID and left.
alignas(4096) u8 data_c[4096]{};
alignas(4096) u8 data_c_replacement[4096]{};
alignas(4096) u8 stack_c[4096]{};
alignas(4096) u8 data_d[4096]{};
alignas(4096) u8 stack_d[4096]{};
alignas(16) u8 secondary_stack[64 * 1024]{};
alignas(4096) std::byte entry_arena[memory::KernelEntryStackPool::arena_bytes]{};
constinit memory::KernelEntryStackPool entry_pool{};
constexpr memory::KernelEntryStackOwner secondary_owner{1, 2};
constinit native::KernelEntryStackView secondary_view{};
constinit ok::smp::SpinLock secondary_lock{};
enum SecondaryPhase : u32
{
    secondary_idle,
    secondary_started,
    secondary_user_loop,
    secondary_done,
    secondary_failed,
};
constinit std::atomic<u32> secondary_phase{};
constinit std::atomic<bool> secondary_stop{};
native::AddressSpace secondary_space{};
struct SecondaryResult
{
    u32 cpu_index{~0u};
    bool syscalls{};
    bool remote_store{};
    native::Trap local_timer{};
    native::Trap ipi{};
    usize timer_traps{};
};
SecondaryResult secondary_result{};

Result<native::Trap> secondary_enter(native::UserFrame &frame)
{
    secondary_lock.lock();
    ok::smp::begin_user(secondary_space.root);
    secondary_lock.unlock();
    auto trap = native::enter_owned(secondary_space, frame, secondary_view);
    ok::smp::end_user();
    return trap;
}
// The user program's two syscalls from its start; the second follows its store
// of `value` at data_va. False on any other outcome.
bool secondary_store(native::UserFrame &frame, u64 value)
{
    native::Trap trap{};
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        auto next = secondary_enter(frame);
        if (!next) return false;
        if ((trap = next.value()).kind != native::TrapKind::timer) break;
    }
    if (trap.kind != native::TrapKind::syscall || native::syscall(frame).number != 0x1234)
        return false;
    native::set_return_value(frame, value);
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        auto next = secondary_enter(frame);
        if (!next) return false;
        if ((trap = next.value()).kind != native::TrapKind::timer) break;
    }
    return trap.kind == native::TrapKind::syscall && native::syscall(frame).number == 0x2345;
}

void secondary_run()
{
    auto &result = secondary_result;
    result.cpu_index = ok::smp::current_cpu();
    auto frame = native::initial_frame(code_va, native::user_end);
    result.syscalls = secondary_store(frame, 13) && data_c[0] == 13;
    if (!result.syscalls) return;
    // The user loop that follows makes no syscall: only the local timer or an
    // IPI can end an entry.
    for (;;)
    {
        auto next = secondary_enter(frame);
        if (!next) return;
        if (next.value().kind == native::TrapKind::timer)
        {
            ++result.timer_traps;
            result.local_timer = next.value();
            secondary_phase.store(secondary_user_loop, std::memory_order_release);
        }
        else if (next.value().kind == native::TrapKind::interrupt)
            result.ipi = next.value();
        else
            return;
        secondary_lock.lock();
        const bool stop = secondary_stop.load(std::memory_order_acquire);
        secondary_lock.unlock();
        if (stop) break;
    }
    frame = native::initial_frame(code_va, native::user_end);
    result.remote_store = secondary_store(frame, 0x31);
    secondary_phase.store(secondary_done, std::memory_order_release);
}

// Skipped with one CPU. The device tree comes straight from the boot handoff;
// this probe does not build the kernel's qualified boot memory.
void check_secondary_cpu()
{
    const auto &handoff = ok::boot::raw_boot_handoff();
    const uptr blob = handoff.captured ? handoff.arguments[0] : 0;
    require(blob != 0 && (blob & 7) == 0, "device tree in the boot handoff");
    const auto *fdt = reinterpret_cast<const u8 *>(blob);
    const u32 size = (u32{fdt[4]} << 24) | (u32{fdt[5]} << 16) | (u32{fdt[6]} << 8) | u32{fdt[7]};
    ok::boot::CpuTopologyMap map{};
    require(size <= ok::boot::max_fdt_bytes &&
                ok::boot::parse_fdt_cpus({reinterpret_cast<const std::byte *>(blob), size},
                                         ok::boot::FdtCpuFlavor::arm, map)
                    .ok(),
            "CPU discovery");
    if (map.count < 2 || map.method == ok::boot::CpuStartMethod::none)
    {
        print("OK_NATIVE_SMP scope=hardware secondary=skipped reason=single-cpu\n");
        return;
    }
    require(ok::boot::mark_boot_cpu(map, ok::arch::smp::current_hardware_id()), "boot CPU in the device tree");
    usize secondary = map.count;
    for (usize i = 0; i < map.count && secondary == map.count; ++i)
        if (i != map.boot_index && map.cpus[i].enabled)
            secondary = i;
    require(secondary < map.count, "an enabled secondary CPU");
    const auto hardware_id = map.cpus[secondary].hardware_id;
    require(ok::arch::smp::prepare_boot_cpu(map).ok(), "boot CPU GIC interface");

    const native::PageMapping mappings_boot[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_d), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_d), native::page_read | native::page_write},
    };
    const native::PageMapping mappings_secondary[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_c), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_c), native::page_read | native::page_write},
    };
    auto boot_result = native::create_space(mappings_boot);
    auto secondary_result_space = native::create_space(mappings_secondary);
    require(boot_result.ok() && secondary_result_space.ok(), "create per-CPU test spaces");
    auto boot_space = boot_result.value();
    secondary_space = secondary_result_space.value();

    // A secondary's block has no entry stack of its own: lease one for it.
    require(entry_pool.activate(entry_arena).ok(), "entry stack pool");
    const auto lease = entry_pool.reserve(secondary_owner);
    require(lease.ok(), "secondary entry stack lease");
    const auto active = entry_pool.activate_entry(lease.value(), secondary_owner, 1);
    require(active.ok(), "secondary entry stack activation");
    secondary_view = active.value();

    auto &record = ok::smp::cpu_record(1);
    record.hardware_id = hardware_id;
    record.stack_lower = reinterpret_cast<uptr>(secondary_stack);
    record.stack_top = reinterpret_cast<uptr>(secondary_stack + sizeof(secondary_stack));
    ok::smp::detail::ipi_sender = &ok::arch::smp::send_ipi;
    require(ok::arch::smp::start(1, hardware_id, record.stack_lower, record.stack_top).ok(), "PSCI CPU_ON");
    require(wait_for([&] { return secondary_phase.load(std::memory_order_acquire) >= secondary_started; }, 2000) &&
                record.reported_id.load(std::memory_order_acquire) == hardware_id,
            "secondary ran C++ and reported its own MPIDR");
    ok::smp::detail::online.store(3, std::memory_order_release);
    require(wait_for([&] { return secondary_phase.load(std::memory_order_acquire) >= secondary_user_loop; }, 2000),
            "secondary entered EL0 and its local timer preempted it");
    const auto boot_tags = native::tlb_tag_report(1);
    const auto secondary_tags = native::tlb_tag_report(2);
    require(secondary_tags.supported && secondary_tags.asid_bits == boot_tags.asid_bits &&
                secondary_tags.invalidations != 0,
            "secondary sized its own ASIDs and entered under one");

    // The boot CPU runs its own space meanwhile.
    store_through_user(boot_space, 17);
    require(data_d[0] == 17 && data_c[0] == 13, "concurrent spaces keep distinct backing");

    // The remap waits until the secondary, kicked by an IPI, left the space.
    // Between two entries the secondary is outside its window for a moment and
    // nothing is waited for: retry until a remap waited.
    const auto received = record.ipis_received.load(std::memory_order_acquire);
    const auto shootdowns = ok::smp::cpu_record(0).shootdowns.load(std::memory_order_relaxed);
    const native::PageMapping replacement{data_va, reinterpret_cast<uptr>(data_c_replacement),
                                          native::page_read | native::page_write};
    bool remapped = true, waited = false, generation_even = true;
    for (usize attempt = 0; attempt < 100 && !waited; ++attempt)
    {
        secondary_lock.lock();
        remapped = native::remap(secondary_space, replacement).ok() && remapped;
        waited = ok::smp::cpu_record(0).shootdowns.load(std::memory_order_relaxed) != shootdowns;
        generation_even = (record.user_generation.load(std::memory_order_acquire) & 1) == 0 && generation_even;
        if (waited) secondary_stop.store(true, std::memory_order_release);
        secondary_lock.unlock();
    }
    require(remapped && waited && generation_even, "remap quiesced the secondary's user window");
    require(wait_for([&] { return secondary_phase.load(std::memory_order_acquire) == secondary_done; }, 2000),
            "secondary finished its user entries");
    const auto &result = secondary_result;
    require(result.cpu_index == 1 && ok::smp::current_cpu() == 0, "per-CPU logical ids");
    require(result.syscalls, "secondary EL0 syscalls and return values");
    require(result.local_timer.kind == native::TrapKind::timer && result.local_timer.vector == 30 &&
                record.local_ticks.load(std::memory_order_relaxed) >= result.timer_traps,
            "local physical timer preempts the secondary");
    require(result.ipi.kind == native::TrapKind::interrupt && result.ipi.vector < 16 &&
                record.ipis_received.load(std::memory_order_acquire) > received,
            "IPI preempts the secondary");
    require(result.remote_store && data_c_replacement[0] == 0x31 && data_c[0] == 13,
            "the secondary's next entry after the remap stores into the new frame");
    const auto after = native::tlb_tag_report(2);
    require(after.invalidations > secondary_tags.invalidations,
            "the secondary invalidated its ASID for the moved generation");
    require(entry_pool.deactivate_entry(lease.value(), secondary_owner, 1).ok() &&
                entry_pool.release(lease.value(), secondary_owner).ok(),
            "release the secondary's entry stack");
    require(native::destroy_space(boot_space).ok() && native::destroy_space(secondary_space).ok(),
            "release per-CPU test spaces");
    if (after.asid_bits == 16)
        print("OK_NATIVE_SMP scope=hardware cpus=2 asid=16");
    else
        print("OK_NATIVE_SMP scope=hardware cpus=2 asid=8");
    print(" secondary_user=pass syscall=pass local_timer=pass ipi=pass shootdown=pass remote_remap=pass\n");
}

void check_fault(native::AddressSpace &space, const u8 *entry, native::TrapKind kind, uptr address, uptr error_class)
{
    auto frame = native::initial_frame(code_va + static_cast<uptr>(entry - ok_native_user_begin), native::user_end);
    const auto trap = next_non_timer(space, frame);
    require(trap.kind == kind, "unexpected CPU fault kind");
    require(trap.fault_address == address, "unexpected hardware FAR_EL1");
    require((trap.error_code >> 26) == error_class, "unexpected hardware exception class");
}
} // namespace

// The secondary's C++ entry after PSCI CPU_ON and its per-CPU setup.
extern "C" [[noreturn]] void ok_smp_secondary_main(ok::smp::CpuId cpu)
{
    auto &record = ok::smp::cpu_record(cpu);
    record.reported_id.store(static_cast<uptr>(ok::arch::smp::current_hardware_id()), std::memory_order_release);
    record.state.store(static_cast<u8>(ok::smp::CpuState::online), std::memory_order_release);
    secondary_phase.store(secondary_started, std::memory_order_release);
    secondary_run();
    if (secondary_phase.load(std::memory_order_acquire) != secondary_done)
        secondary_phase.store(secondary_failed, std::memory_order_release);
    for (;;)
        ok::arch::smp::wait_local();
}

// smp.cpp's discover() reads the kernel's qualified boot memory, which this
// probe does not build; check_secondary_cpu parses the device tree itself.
const ok::boot::KernelBootMemory &ok::boot::kernel_boot_memory()
{
    static const KernelBootMemory none{};
    return none;
}

extern "C" void ok_platform_console_write_char(char value)
{
    print(std::string_view(&value, 1));
}
extern "C" void *memset(void *destination, int value, usize count)
{
    auto *bytes = static_cast<u8 *>(destination);
    for (usize i = 0; i < count; ++i)
        bytes[i] = static_cast<u8>(value);
    return destination;
}
extern "C" void *memcpy(void *destination, const void *source, usize count)
{
    auto *out_bytes = static_cast<u8 *>(destination);
    const auto *in_bytes = static_cast<const u8 *>(source);
    for (usize i = 0; i < count; ++i)
        out_bytes[i] = in_bytes[i];
    return destination;
}
// The device tree parser's string compares.
extern "C" int memcmp(const void *left, const void *right, usize size)
{
    const auto *a = static_cast<const u8 *>(left), *b = static_cast<const u8 *>(right);
    for (usize i = 0; i < size; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}
extern "C" void *memchr(const void *from, int value, usize size)
{
    const auto *in = static_cast<const u8 *>(from);
    for (usize i = 0; i < size; ++i)
        if (in[i] == static_cast<u8>(value))
            return const_cast<u8 *>(in + i);
    return nullptr;
}

extern "C" [[noreturn]] void kernel_main()
{
    print("OK_NATIVE_TEST boot=begin\n");
    require(native::initialize().ok(), "native initialize");
    require(static_cast<usize>(ok_native_user_end - ok_native_user_begin) < sizeof(code), "user test text capacity");
    memcpy(code, ok_native_user_begin, ok_native_user_end - ok_native_user_begin);
    const native::PageMapping mappings_a[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_a), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_a), native::page_read | native::page_write},
    };
    const native::PageMapping mappings_b[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_b), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_b), native::page_read | native::page_write},
    };
    auto result_a = native::create_space(mappings_a);
    auto result_b = native::create_space(mappings_b);
    require(result_a.ok() && result_b.ok(), "create hardware address spaces");
    auto space_a = result_a.value();
    auto space_b = result_b.value();
    require(!native::user_range(space_a, 0x40080000, 1), "reject kernel pointer");
    require(!native::user_range(space_a, data_va + 4095, 2), "reject partially unmapped buffer");
    require(!native::user_range(space_a, native::user_end - 1, static_cast<usize>(-1)), "reject overflowed buffer");
    require(!native::user_range(space_a, code_va, 1, true), "reject write to code");
    const u8 marker = 0xa5;
    require(native::copy_to_user(space_a, data_va + 128, &marker, 1).ok() && data_a[128] == marker,
            "copy user data through safe physical alias");
    print("OK_NATIVE_TEST user_copy=pass\n");

    auto frame_a = native::initial_frame(code_va, native::user_end);
    frame_a.fs_base = data_va + 128;
    uptr boot_root;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(boot_root));
    auto trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall, "EL0 SVC trap");
    const auto call = native::syscall(frame_a);
    require(call.number == 0x1234 && call.arguments[0] == 23 && call.arguments[1] == 42 &&
                call.arguments[2] == data_va + 128 && (frame_a.flags & 15) == 0,
            "actual EL0, TLS and syscall registers");
    require(frame_a.registers[19] == 0x1122334455667788ULL && frame_a.registers[20] == 0x8877665544332211ULL,
            "callee-saved user registers");
    require(frame_a.fp_state[0] == 0x5a && frame_a.fp_state[496] == 0xa5 &&
                (frame_a.registers[31] & 0xffffffff) == 0x400000,
            "SIMD and FPCR user state saved before kernel C++");
    print("OK_NATIVE_TEST el0=pass syscall=pass registers=pass\n");
    native::set_return_value(frame_a, 7);
    trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame_a).number == 0x2345,
            "syscall return resumes user instruction");
    require(data_a[0] == 7 && data_a[16] == 0x5a && data_a[32] == 0xa5, "user return register and SIMD state restored");

    auto frame_b = native::initial_frame(code_va, native::user_end);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall, "second process syscall");
    native::set_return_value(frame_b, 11);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall && data_b[0] == 11 && data_a[0] == 7,
            "same virtual address has distinct physical process storage");
    print("OK_NATIVE_TEST address_spaces=pass simd=pass\n");
    check_tag_churn(space_b, data_b);
    auto timer = native::enter(space_a, frame_a);
    require(timer.ok() && timer.value().kind == native::TrapKind::timer && timer.value().vector == 30,
            "architectural timer preempts non-cooperative user loop");
    print("OK_NATIVE_TEST timer_preemption=pass\n");

    check_fault(space_a, ok_native_user_kernel_read, native::TrapKind::page_fault, 0x40080000, 0x24);
    check_fault(space_a, ok_native_user_readonly_write, native::TrapKind::page_fault, code_va, 0x24);
    check_fault(space_a, ok_native_user_nx, native::TrapKind::page_fault, data_va, 0x20);
    check_fault(space_a, ok_native_user_unmapped, native::TrapKind::page_fault, 0x30000000, 0x24);
    check_fault(space_a, ok_native_user_invalid, native::TrapKind::exception, 0, 0);
    print("OK_NATIVE_TEST supervisor_isolation=pass readonly=pass nx=pass faults=pass\n");
    uptr restored_root;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(restored_root));
    require(restored_root == boot_root, "restore kernel TTBR0 after user fault");
    require(native::destroy_space(space_a).ok() && native::destroy_space(space_b).ok(),
            "release native address spaces");
    require(!native::user_range(space_a, data_va, 1), "stale address-space handle rejected");
    check_secondary_cpu();
    print("OK_NATIVE_TEST_PASS scope=hardware architecture=aarch64\n");
    finish(true);
}
