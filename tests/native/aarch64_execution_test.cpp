#include "ok/arch/execution.hpp"
#include "ok/arch/smp.hpp"
#include "ok/boot/boot_memory.hpp"
#include "ok/boot/cpu_topology.hpp"
#include "ok/boot/fdt.hpp"
#include "ok/boot/raw_handoff.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/kernel_lock.hpp"
#include "ok/smp/kernel_section.hpp"
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
template <typename Done> bool wait_for(Done done, u64 milliseconds = 2000)
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
constinit std::atomic<bool> secondary_stop{}, secondary_identity{};
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
    require(ok::boot::mark_boot_cpu(map, ok::arch::smp::current_hardware_id()) &&
                (map.method == ok::boot::CpuStartMethod::psci_hvc || map.method == ok::boot::CpuStartMethod::psci_smc),
            "boot CPU and a PSCI start method in the device tree");
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
                record.reported_id.load(std::memory_order_acquire) == hardware_id &&
                record.state.load(std::memory_order_acquire) == static_cast<u8>(ok::smp::CpuState::online) &&
                secondary_identity.load(std::memory_order_relaxed),
            "secondary ran C++ with its own block and reported its own MPIDR");
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

// SPI 47 (ID 79), the last virtio-mmio slot, has no device behind it, so
// only the probe makes it pending (GICD_ISPENDR). Edge-triggered: the claim
// takes the pending state.
constexpr interrupt::IrqLine spi_line = 79;
constinit std::atomic<u32> spi_serviced{};
void raise_spi()
{
    *reinterpret_cast<volatile u32 *>(0x08000000 + 0x200 + 4 * (spi_line / 32)) = 1u << (spi_line % 32);
}
void spi_service(void *, interrupt::IrqLine)
{
    spi_serviced.fetch_add(1, std::memory_order_relaxed);
}
[[nodiscard]] usize delivered_on(ok::smp::CpuId cpu)
{
    return interrupt::platform_irq_cpu_statistics(cpu).delivered;
}

// Whether the top half that ended a wait also sent its IPI: the target took
// it, or the sender sent none because the target was between two waits then,
// which loses no wake (a waiter checks for pending lines after it publishes
// itself). The caller then tries again. An IPI sent but not taken fails.
enum class WakeIpi
{
    received,
    not_sent,
};
template <typename Serve>
WakeIpi wake_ipi(const ok::smp::CpuRecord &target, uptr received, const ok::smp::CpuRecord &sender, uptr sent,
                 Serve serve)
{
    if (wait_for(
            [&] {
                serve();
                return target.ipis_received.load(std::memory_order_acquire) != received;
            },
            50))
        return WakeIpi::received;
    require(sender.ipis_sent.load(std::memory_order_acquire) == sent, "a wake IPI was sent but never taken");
    return WakeIpi::not_sent;
}
constexpr usize wake_attempts = 8;

// CPU 1's device share after its user entries above, by phase: EL0 entries
// during which it makes the SPI routed to it pending from its kernel side;
// idle waits; a device wait holding the kernel lock, as a driver inside a
// syscall makes one; a retirement, as a hotplug path makes one; a kernel
// section polling a device.
enum DevicePhase : u32
{
    device_none,
    device_user,
    device_user_stop,
    device_user_done,
    device_wait,
    device_woken,
    device_retire,
    device_retired,
    device_section,
    device_section_stop,
    device_section_done,
};
constinit std::atomic<u32> device_phase{};
constinit std::atomic<bool> raise_request{}, accepted{}, section_wait{};
constinit std::atomic<u32> device_traps{}, device_other{}, device_syscalls{};
constinit std::atomic<usize> device_drained{}, device_moved{};
constexpr memory::KernelEntryStackOwner device_owner{1, 1};
constinit memory::KernelEntryStackHandle device_handle{};

void secondary_device_user()
{
    auto frame = native::initial_frame(code_va, native::user_end);
    const auto stack = entry_pool.activate_entry(device_handle, device_owner, 1);
    if (!stack)
        device_other.fetch_add(1, std::memory_order_relaxed);
    while (stack && device_phase.load(std::memory_order_acquire) == device_user)
    {
        const auto next = native::enter_owned(secondary_space, frame, stack.value());
        if (!next)
        {
            device_other.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        const auto &trap = next.value();
        if (trap.kind == native::TrapKind::syscall)
        {
            device_syscalls.fetch_add(1, std::memory_order_relaxed);
            native::set_return_value(frame, 0);
        }
        else if (trap.kind == native::TrapKind::interrupt && trap.vector == spi_line)
            device_traps.fetch_add(1, std::memory_order_relaxed);
        else if (trap.kind != native::TrapKind::timer && trap.kind != native::TrapKind::interrupt)
            device_other.fetch_add(1, std::memory_order_relaxed);
        if (raise_request.exchange(false, std::memory_order_acq_rel))
        {
            // Long enough for the boot CPU to halt in its wait first. The
            // SPI reaches this CPU at its next EL0 entry.
            static_cast<void>(wait_for([] { return false; }, 2));
            raise_spi();
        }
    }
    if (stack && !entry_pool.deactivate_entry(device_handle, device_owner, 1))
        device_other.fetch_add(1, std::memory_order_relaxed);
    device_phase.store(device_user_done, std::memory_order_release);
}

void secondary_devices()
{
    for (;;)
    {
        const auto now = device_phase.load(std::memory_order_acquire);
        if (now == device_user)
            secondary_device_user();
        else if (now == device_wait)
        {
            // A driver inside a syscall: it holds the kernel lock across the
            // wait and drains its own line.
            ok::smp::kernel_lock.lock();
            while (!interrupt::platform_irq_pending())
                if (!interrupt::wait_for_interrupt())
                {
                    device_other.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            device_drained.store(interrupt::drain_platform_irqs(), std::memory_order_relaxed);
            ok::smp::kernel_lock.unlock();
            device_phase.store(device_woken, std::memory_order_release);
        }
        else if (now == device_retire)
        {
            raise_spi();
            accepted.store(wait_for([] {
                u64 status;
                asm volatile("mrs %0, isr_el1" : "=r"(status));
                return (status & 0x80) != 0;
            }));
            device_moved.store(interrupt::retarget_platform_irqs_away(1), std::memory_order_relaxed);
            device_phase.store(device_retired, std::memory_order_release);
        }
        else if (now == device_section)
        {
            // A kernel section whose driver polls, as an ext4 fsync section
            // polls virtio-blk: no kernel lock and no bottom halves, but every
            // pass serves this CPU's controller, through the interrupt wait
            // (which refuses inside a section) or the poll loop's
            // serve_while_polling. The SPI is raised from here once the boot
            // CPU halted in its wait.
            ok::smp::bkl_lock();
            ok::smp::enter_section(1, 1);
            while (device_phase.load(std::memory_order_acquire) == device_section)
            {
                if (raise_request.exchange(false, std::memory_order_acq_rel))
                {
                    static_cast<void>(wait_for([] { return false; }, 2));
                    raise_spi();
                }
                if (!section_wait.load(std::memory_order_relaxed))
                    ok::smp::serve_while_polling();
                else if (interrupt::wait_for_interrupt())
                    device_other.fetch_add(1, std::memory_order_relaxed);
            }
            ok::smp::leave_section(0);
            ok::smp::bkl_unlock();
            device_phase.store(device_section_done, std::memory_order_release);
        }
        else
            ok::arch::smp::wait_local();
    }
}

// The interrupt service on the GIC. With one CPU, the boot CPU's interrupt
// wait takes the SPI. With CPU 1 online (check_secondary_cpu started it
// through PSCI), the SPI is routed to CPU 1 through GICD_ITARGETSR and taken
// there in EL0 and in its idle wait, and the IPI of its top half ends the boot
// CPU's interrupt wait. The boot CPU then takes the SPI while it spins for the
// kernel lock CPU 1 holds across a device wait
// (lock_kernel_serving_interrupts), and its top half's IPI ends that wait.
// Last, CPU 1 retargets its line away as before going offline.
void check_device_interrupts()
{
    const bool secondary = secondary_phase.load(std::memory_order_acquire) == secondary_done;
    ok::smp::detail::ipi_wake = []() noexcept { ok_interrupt_wake(); };
    require(interrupt::start_platform_irqs().ok(), "interrupt service");
    if (!secondary)
    {
        require(interrupt::bind_platform_irq(spi_line, interrupt::IrqTrigger::edge, spi_service, nullptr).ok() &&
                    interrupt::platform_irq_target(spi_line) == 0,
                "bind the SPI on the boot CPU");
        raise_spi();
        while (!interrupt::platform_irq_pending())
            require(interrupt::wait_for_interrupt(), "boot CPU interrupt wait");
        require(delivered_on(0) == 1 && interrupt::drain_platform_irqs() == 1 && spi_serviced.load() == 1,
                "the boot CPU's wait runs the SPI's top half");
        print("OK_NATIVE_GIC scope=hardware cpus=1 boot_wait=pass\n");
        return;
    }
    auto &record = ok::smp::cpu_record(1);
    auto &boot = ok::smp::cpu_record(0);
    const native::PageMapping mappings[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_c), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_c), native::page_read | native::page_write},
    };
    auto space = native::create_space(mappings);
    require(space.ok(), "create the secondary device space");
    secondary_space = space.value();
    auto handle = entry_pool.reserve(device_owner);
    require(handle.ok(), "reserve CPU 1's device entry stack");
    device_handle = handle.value();

    require(interrupt::bind_platform_irq(spi_line, interrupt::IrqTrigger::edge, spi_service, nullptr).ok() &&
                interrupt::retarget_platform_irq(spi_line, 1).ok() && interrupt::platform_irq_target(spi_line) == 1,
            "route the SPI to CPU 1");
    device_phase.store(device_user, std::memory_order_release);
    require(wait_for([] { return device_syscalls.load() >= 2; }), "CPU 1 reaches its EL0 loop");
    u32 serviced = 0;
    for (u32 attempt = 1;; ++attempt)
    {
        const auto boot_ipis = boot.ipis_received.load();
        const auto sent = record.ipis_sent.load();
        raise_request.store(true, std::memory_order_release);
        while (!interrupt::platform_irq_pending())
            require(interrupt::wait_for_interrupt(), "boot CPU interrupt wait");
        require(wait_for([&] { return device_traps.load() == attempt; }) && delivered_on(1) == attempt &&
                    delivered_on(0) == 0,
                "CPU 1's EL0 trap runs the top half");
        const auto woken = wake_ipi(boot, boot_ipis, record, sent, [] { interrupt::serve_local_interrupts(); });
        require(interrupt::drain_platform_irqs() == 1 && spi_serviced.load() == ++serviced &&
                    !interrupt::platform_irq_pending(),
                "the boot CPU drains CPU 1's line");
        if (woken == WakeIpi::received)
            break;
        require(attempt < wake_attempts, "the top half wakes the boot CPU with an IPI");
    }
    device_phase.store(device_user_stop, std::memory_order_release);
    require(wait_for([] { return device_phase.load(std::memory_order_acquire) == device_user_done; }) &&
                device_other.load() == 0,
            "CPU 1 left EL0");

    // CPU 1's idle wait claims the SPI and runs the top half.
    const auto secondary_delivered = delivered_on(1);
    raise_spi();
    while (!interrupt::platform_irq_pending())
        require(interrupt::wait_for_interrupt(), "boot CPU interrupt wait");
    require(delivered_on(1) == secondary_delivered + 1 && delivered_on(0) == 0 &&
                interrupt::drain_platform_irqs() == 1 && spi_serviced.load() == ++serviced,
            "CPU 1's idle wait runs the top half");

    // CPU 1 holds the kernel lock across a device wait; the SPI now goes to
    // the boot CPU, which takes it while it spins for that lock.
    require(interrupt::retarget_platform_irq(spi_line, 0).ok() && interrupt::platform_irq_target(spi_line) == 0,
            "route the SPI to the boot CPU");
    for (u32 attempt = 1;; ++attempt)
    {
        const auto waits = interrupt::platform_irq_cpu_statistics(1).waits;
        const auto secondary_serviced = interrupt::platform_irq_cpu_statistics(1).serviced;
        const auto secondary_ipis = record.ipis_received.load();
        const auto sent = boot.ipis_sent.load();
        device_phase.store(device_wait, std::memory_order_release);
        require(wait_for([&] { return interrupt::platform_irq_cpu_statistics(1).waits != waits; }),
                "CPU 1 halts in its device wait");
        require(!ok::smp::kernel_lock.try_lock(), "CPU 1 holds the kernel lock while it waits");
        raise_spi();
        interrupt::lock_kernel_serving_interrupts();
        ok::smp::kernel_lock.unlock();
        require(delivered_on(0) == attempt && wait_for([] { return device_phase.load() == device_woken; }) &&
                    device_drained.load() == 1 && spi_serviced.load() == ++serviced &&
                    interrupt::platform_irq_cpu_statistics(1).serviced == secondary_serviced + 1 &&
                    device_other.load() == 0,
                "the boot CPU's top half ends CPU 1's locked device wait");
        // CPU 1 takes an IPI still pending in its next wait_local().
        const auto woken = wake_ipi(record, secondary_ipis, boot, sent, [] {});
        if (woken == WakeIpi::received)
            break;
        require(attempt < wake_attempts, "the boot CPU's top half ends CPU 1's device wait with an IPI");
    }

    // CPU 1 leaves as a hotplug path would, with the SPI pending at its CPU
    // interface. It runs one top half, on CPU 1 before
    // retarget_platform_irqs_away() returns or on the boot CPU once the
    // target moved, and the line then reaches the boot CPU only.
    require(interrupt::retarget_platform_irq(spi_line, 1).ok(), "route the SPI to CPU 1 again");
    const auto delivered_before = delivered_on(0) + delivered_on(1);
    device_phase.store(device_retire, std::memory_order_release);
    require(wait_for([] { return device_phase.load(std::memory_order_acquire) == device_retired; }) &&
                accepted.load() && device_moved.load() == 1 && interrupt::platform_irq_target(spi_line) == 0,
            "CPU 1 retargets its line away");
    require(wait_for([] {
                interrupt::serve_local_interrupts();
                return interrupt::platform_irq_pending();
            }) && interrupt::drain_platform_irqs() == 1 && spi_serviced.load() == ++serviced &&
                delivered_on(0) + delivered_on(1) == delivered_before + 1,
            "the SPI CPU 1 held runs one top half");
    const auto boot_delivered = delivered_on(0), secondary_after = delivered_on(1);
    raise_spi();
    require(wait_for([&] {
                interrupt::serve_local_interrupts();
                return delivered_on(0) == boot_delivered + 1;
            }) && interrupt::drain_platform_irqs() == 1 && spi_serviced.load() == ++serviced &&
                delivered_on(1) == secondary_after,
            "the SPI reaches the boot CPU after the retarget");
    require(entry_pool.release(device_handle, device_owner).ok() && native::destroy_space(secondary_space).ok(),
            "release CPU 1's device entry stack and space");
    print("OK_NATIVE_GIC scope=hardware cpus=2 user_top_half=pass wake_ipi=pass idle_top_half=pass "
          "kernel_lock_wait=pass spin_serve=pass retarget_away=pass\n");

    // The boot CPU holds the kernel lock across a device wait for the SPI
    // routed to CPU 1, while CPU 1 polls a device inside a kernel section
    // with interrupts masked. The section's poll runs the top half (first
    // through its refused interrupt wait, then through serve_while_polling),
    // whose IPI ends the boot CPU's wait; the boot CPU drains under the lock.
    ok::smp::section_commit_handler = [](ok::smp::SectionRecord &) noexcept {};
    ok::smp::spin_service = interrupt::serve_local_interrupts;
    require(interrupt::retarget_platform_irq(spi_line, 1).ok() && interrupt::platform_irq_target(spi_line) == 1,
            "route the SPI to CPU 1 for its section");
    for (u32 mode = 0; mode < 2; ++mode)
        for (u32 attempt = 1;; ++attempt)
        {
            const auto boot_delivered = delivered_on(0), secondary_delivered = delivered_on(1);
            const auto boot_ipis = boot.ipis_received.load();
            const auto sent = record.ipis_sent.load();
            section_wait.store(mode == 0, std::memory_order_relaxed);
            device_phase.store(device_section, std::memory_order_release);
            require(wait_for([] { return ok::smp::in_section(1); }),
                    "CPU 1 polls inside a kernel section");
            ok::smp::bkl_lock();
            raise_request.store(true, std::memory_order_release);
            require(wait_for([] {
                        if (!interrupt::platform_irq_pending())
                            static_cast<void>(interrupt::wait_for_interrupt());
                        return interrupt::platform_irq_pending();
                    }),
                    "a polling section serves the line a kernel-lock holder waits for");
            require(delivered_on(1) == secondary_delivered + 1 && delivered_on(0) == boot_delivered &&
                        interrupt::drain_platform_irqs() == 1 && spi_serviced.load() == ++serviced,
                    "CPU 1's section runs the top half; the lock holder drains");
            const auto woken = wake_ipi(boot, boot_ipis, record, sent, [] { interrupt::serve_local_interrupts(); });
            ok::smp::bkl_unlock();
            device_phase.store(device_section_stop, std::memory_order_release);
            require(wait_for([] { return device_phase.load(std::memory_order_acquire) == device_section_done; }) &&
                        !ok::smp::in_section(1) && device_other.load() == 0,
                    "CPU 1 leaves its section");
            if (woken == WakeIpi::received)
                break;
            require(attempt < wake_attempts, "the section's top half wakes the lock holder with an IPI");
        }
    print("OK_NATIVE_GIC scope=hardware cpus=2 section_wait_serve=pass section_poll_serve=pass\n");
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
    secondary_identity.store(ok::smp::current_cpu() == cpu, std::memory_order_relaxed);
    record.reported_id.store(static_cast<uptr>(ok::arch::smp::current_hardware_id()), std::memory_order_release);
    record.state.store(static_cast<u8>(ok::smp::CpuState::online), std::memory_order_release);
    secondary_phase.store(secondary_started, std::memory_order_release);
    secondary_run();
    if (secondary_phase.load(std::memory_order_acquire) != secondary_done)
        secondary_phase.store(secondary_failed, std::memory_order_release);
    else
        secondary_devices();
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
    check_device_interrupts();
    print("OK_NATIVE_TEST_PASS scope=hardware architecture=aarch64\n");
    finish(true);
}
