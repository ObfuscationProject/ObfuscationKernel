#include "ok/arch/clock.hpp"
#include "ok/arch/execution.hpp"
#include "ok/arch/smp.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/smp/bringup.hpp"
#include "execution_table_pages.hpp"
#include "../../src/arch/tlb_tags.hpp"
#include "../../src/arch/x86_64/ioapic.hpp"
#include "../../src/interrupt/isa_irq.hpp"

#include <atomic>

using namespace ok;
namespace native = ok::arch::execution;

extern "C"
{
    extern const u8 ok_native_user_begin[], ok_native_user_end[];
    extern const u8 ok_native_user_kernel_read[], ok_native_user_readonly_write[];
    extern const u8 ok_native_user_nx[], ok_native_user_unmapped[], ok_native_user_invalid[];
    extern const u8 ok_native_user_dense[];
}
namespace
{
alignas(4096) u8 code[4096]{};
alignas(4096) u8 data_a[4096]{};
alignas(4096) u8 data_b[4096]{};
alignas(4096) u8 stack_a[4096]{};
alignas(4096) u8 stack_b[4096]{};
constexpr usize dense_pages = 192;
constexpr uptr dense_va = native::user_begin + 0x01000000;
alignas(4096) u8 dense_data[dense_pages][4096]{};
alignas(4096) u8 dense_replacement[4096]{};
std::array<native::PageMapping, dense_pages + 2> dense_mappings{};
std::array<native::PageMapping, dense_pages + 1> sparse_mappings{};
constexpr uptr code_va = native::user_begin;
constexpr uptr data_va = native::user_begin + 8192;
constexpr uptr stack_va = native::user_end - 4096;

void out(u16 port, u8 value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
u8 in(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
void print(std::string_view text)
{
    for (const auto value : text)
    {
        while ((in(0x3fd) & 0x20) == 0)
        {
        }
        out(0x3f8, static_cast<u8>(value));
    }
}
[[noreturn]] void finish(bool pass)
{
    const u32 value = pass ? 0x10 : 0x11;
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(u16{0xf4}));
    for (;;)
        asm volatile("cli; hlt");
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
template <typename Done> bool wait_for(Done done, u64 timeout_ns)
{
    const auto start = ok::arch::monotonic_nanoseconds();
    while (!done())
    {
        const auto now = ok::arch::monotonic_nanoseconds();
        if (!start || !now || now.value() - start.value() >= timeout_ns)
            return done();
        ok::arch::ArchOperations::spin_pause();
    }
    return true;
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

// More spaces than a CPU has PCID slots, created and retired in turn like
// mmap churn: with PCIDs the tags roll over and are reused, each space stores
// into its own frame (three frames rotate), and the next entry after a remap
// stores into the new frame. Without PCIDs every entry flushes and no tag
// rolls over. This runs the entry paths; QEMU TCG has no PCID, and a missing
// invalidation is detected only by the hosted model (scripts/test_tlb_tags.py).
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
    // With PCIDs, the first of the two entries after the remap loads CR3
    // without the keep bit (the generation moved) and the second keeps it.
    require(after.invalidations == before_remap.invalidations + (after.pcid ? 1 : 0),
            "one PCID invalidation for the remap");
    require(after.supported && (after.pcid ? after.asid_bits == 12 && after.rollovers >= before.rollovers + 2
                                           : after.asid_bits == 0 && after.rollovers == 0),
            "PCID tags rolled over, or every entry flushed");
    if (after.pcid)
        print("OK_NATIVE_TLB scope=hardware asid=12 pcid=on churn=pass rollover=pass remap=pass\n");
    else
        print("OK_NATIVE_TLB scope=hardware asid=none pcid=off churn=pass rollover=untagged remap=pass\n");
}

// A secondary CPU runs ring-3 code in its own space while the boot CPU runs in
// another. `secondary_lock` stands in for the kernel lock: a CPU opens its
// user window under it, and a translation change holds it. After the boot CPU
// remapped the secondary's data page (a shootdown), the secondary enters that
// space again and must store into the new frame: the path of a CPU that cached
// the old translation (under its PCID, where it has them) and left.
alignas(4096) u8 data_c[4096]{};
alignas(4096) u8 data_c_replacement[4096]{};
alignas(4096) u8 stack_c[4096]{};
alignas(16) u8 secondary_stack[64 * 1024]{};
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
    auto trap = native::enter(secondary_space, frame);
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
    const auto call = native::syscall(frame);
    if (trap.kind != native::TrapKind::syscall || call.number != 0x1234 || call.arguments[0] != 0x23 ||
        call.arguments[1] != 42)
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

// Two CPUs in ring 3 at once on per-CPU entry state: syscalls, the local APIC
// timer and an IPI on the secondary; the PIT through LINT0 on the boot CPU; a
// remap that waits until the secondary left the space, and the secondary's
// next entry into it. Skipped on one CPU.
void check_secondary_cpu()
{
    ok::boot::CpuTopologyMap map{};
    require(ok::arch::smp::discover(map).ok(), "CPU discovery");
    if (map.method != ok::boot::CpuStartMethod::x86_sipi)
    {
        print("OK_NATIVE_SMP scope=hardware secondary=skipped reason=single-cpu\n");
        return;
    }
    usize secondary = map.count;
    for (usize i = 0; i < map.count && secondary == map.count; ++i)
        if (i != map.boot_index && map.cpus[i].enabled)
            secondary = i;
    require(secondary < map.count, "an enabled secondary CPU");
    const auto hardware_id = map.cpus[secondary].hardware_id;
    require(ok::arch::initialize_clock().ok(), "calibrated TSC clock");
    require(ok::arch::smp::prepare_boot_cpu(map).ok(), "boot CPU local APIC and timer calibration");

    const native::PageMapping mappings_boot[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_a), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_a), native::page_read | native::page_write},
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

    auto &record = ok::smp::cpu_record(1);
    record.hardware_id = hardware_id;
    record.stack_lower = reinterpret_cast<uptr>(secondary_stack);
    record.stack_top = reinterpret_cast<uptr>(secondary_stack + sizeof(secondary_stack));
    ok::smp::detail::ipi_sender = &ok::arch::smp::send_ipi;
    require(ok::arch::smp::start(1, hardware_id, record.stack_lower, record.stack_top).ok(), "issue INIT-SIPI");
    require(wait_for([&] { return secondary_phase.load(std::memory_order_acquire) >= secondary_started; },
                     2'000'000'000) &&
                record.reported_id.load(std::memory_order_acquire) == hardware_id,
            "secondary ran C++ and reported its own APIC id");
    ok::smp::detail::online.store(3, std::memory_order_release);
    require(wait_for([&] { return secondary_phase.load(std::memory_order_acquire) >= secondary_user_loop; },
                     2'000'000'000),
            "secondary entered ring 3 and its local timer preempted it");
    const auto boot_tags = native::tlb_tag_report(1);
    const auto secondary_tags = native::tlb_tag_report(2);
    require(secondary_tags.supported && secondary_tags.asid_bits == boot_tags.asid_bits &&
                secondary_tags.pcid == boot_tags.pcid && (secondary_tags.invalidations != 0) == secondary_tags.pcid,
            "secondary configured its own PCIDs, or stays untagged like the boot CPU");

    // The boot CPU runs its own space meanwhile; the PIT still arrives as
    // ExtINT through LINT0 with the local APIC enabled.
    auto frame = native::initial_frame(code_va, native::user_end);
    auto trap = next_non_timer(boot_space, frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame).number == 0x1234,
            "boot CPU syscall while the secondary runs user code");
    native::set_return_value(frame, 17);
    trap = next_non_timer(boot_space, frame);
    require(trap.kind == native::TrapKind::syscall && data_a[0] == 17 && data_c[0] == 13,
            "concurrent spaces keep distinct backing");
    auto timer = native::enter(boot_space, frame);
    require(timer.ok() && timer.value().kind == native::TrapKind::timer && timer.value().vector == 32,
            "PIT preempts the boot CPU through LINT0");

    // The remap waits until the secondary, kicked by an IPI, left the space.
    const auto shootdowns = ok::smp::cpu_record(0).shootdowns.load(std::memory_order_relaxed);
    const auto received = record.ipis_received.load(std::memory_order_acquire);
    secondary_lock.lock();
    const native::PageMapping replacement{data_va, reinterpret_cast<uptr>(data_c_replacement),
                                          native::page_read | native::page_write};
    const bool remapped = native::remap(secondary_space, replacement).ok();
    const bool waited = ok::smp::cpu_record(0).shootdowns.load(std::memory_order_relaxed) == shootdowns + 1;
    const bool generation_even = (record.user_generation.load(std::memory_order_acquire) & 1) == 0;
    secondary_stop.store(true, std::memory_order_release);
    secondary_lock.unlock();
    require(remapped && waited && generation_even, "remap quiesced the secondary's user window");
    require(wait_for([&] { return secondary_phase.load(std::memory_order_acquire) == secondary_done; },
                     2'000'000'000),
            "secondary finished its user entries");
    const auto &result = secondary_result;
    require(result.cpu_index == 1 && ok::smp::current_cpu() == 0, "per-CPU logical ids");
    require(result.syscalls, "secondary CPL3 syscalls and return values");
    require(result.local_timer.kind == native::TrapKind::timer && result.local_timer.vector == 0xef &&
                record.local_ticks.load(std::memory_order_relaxed) >= result.timer_traps,
            "local APIC timer preempts the secondary");
    require(result.ipi.kind == native::TrapKind::interrupt && result.ipi.vector == 0xf0 &&
                record.ipis_received.load(std::memory_order_acquire) > received,
            "IPI preempts the secondary");
    require(result.remote_store && data_c_replacement[0] == 0x31 && data_c[0] == 13,
            "the secondary's next entry after the remap stores into the new frame");
    const auto after = native::tlb_tag_report(2);
    require(after.pcid ? after.invalidations > secondary_tags.invalidations : after.invalidations == 0,
            "the secondary invalidated its PCID for the moved generation");
    require(result.local_timer.kernel_stack_pointer != 0 && trap.kernel_stack_pointer != 0 &&
                result.local_timer.kernel_stack_pointer != timer.value().kernel_stack_pointer,
            "each CPU traps onto its own kernel stack");
    require(native::destroy_space(boot_space).ok() && native::destroy_space(secondary_space).ok(),
            "release per-CPU test spaces");
    // The start ran the TSC handshake before the secondary's C++ entry and
    // admitted only a verified clock.
    const auto tsc = ok::arch::smp::clock_sync_name();
    // Compared by hand: this probe links no memcmp.
    const auto is = [&](std::string_view name) {
        if (tsc.size() != name.size())
            return false;
        for (usize i = 0; i < name.size(); ++i)
            if (tsc[i] != name[i])
                return false;
        return true;
    };
    require(is("sync") || is("adjusted") || is("offset"), "verified TSC handshake with the secondary");
    print("OK_NATIVE_SMP scope=hardware cpus=2 secondary_user=pass syscall=pass local_timer=pass ipi=pass "
          "shootdown=pass remote_remap=pass boot_pit=pass tsc=");
    print(tsc);
    print("\n");
}

// The 16550's transmitter-empty interrupt (ISA IRQ 4) is a device line the
// probe raises at will: enabling it in IER raises the line while THR is
// empty, and the bottom half disables it again. Nothing is printed while it
// is enabled, since every transmitted byte would raise it once more.
namespace ioapic = ok::arch::x86::ioapic;
constexpr u8 uart_irq = 4;
constinit std::atomic<u32> uart_serviced{};
void raise_uart_line()
{
    out(0x3f9, 0x02);
}
void uart_service(void *, interrupt::IrqLine)
{
    out(0x3f9, 0);
    uart_serviced.fetch_add(1, std::memory_order_relaxed);
}
[[nodiscard]] usize delivered_on(ok::smp::CpuId cpu)
{
    return interrupt::platform_irq_cpu_statistics(cpu).delivered;
}

// CPU 1's share: user entries during which it raises the line routed to it
// from its kernel side, then idle waits, then a device wait as a driver
// inside a syscall would make one.
enum DevicePhase : u32
{
    device_none,
    device_user,
    device_user_done,
    device_idle,
    device_wait,
    device_woken,
};
constinit std::atomic<u32> device_phase{};
constinit std::atomic<bool> device_raise{}, device_stop{};
constinit std::atomic<u32> device_traps{}, device_other{};
constinit interrupt::IrqLine uart_line{};
native::AddressSpace device_space{};

void secondary_devices()
{
    while (device_phase.load(std::memory_order_acquire) != device_user)
        ok::arch::smp::wait_local();
    auto frame = native::initial_frame(code_va, native::user_end);
    while (!device_stop.load(std::memory_order_acquire))
    {
        const auto next = native::enter(device_space, frame);
        if (!next)
        {
            device_other.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        const auto &trap = next.value();
        if (trap.kind == native::TrapKind::interrupt && trap.vector == ioapic::vector_base + uart_line)
            device_traps.fetch_add(1, std::memory_order_relaxed);
        else if (trap.kind != native::TrapKind::syscall && trap.kind != native::TrapKind::timer &&
                 trap.kind != native::TrapKind::interrupt)
            device_other.fetch_add(1, std::memory_order_relaxed);
        if (device_raise.exchange(false, std::memory_order_acq_rel))
        {
            // Long enough for the boot CPU to halt in its wait first. The
            // line reaches this CPU at its next user entry.
            static_cast<void>(wait_for([] { return false; }, 2'000'000));
            raise_uart_line();
        }
    }
    device_phase.store(device_user_done, std::memory_order_release);
    // One device wait each time the boot CPU asks for one.
    for (;;)
    {
        while (device_phase.load(std::memory_order_acquire) != device_wait)
            ok::arch::smp::wait_local();
        while (!interrupt::platform_irq_pending())
            if (!interrupt::wait_for_interrupt())
            {
                device_other.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        device_phase.store(device_woken, std::memory_order_release);
    }
}

// Whether the top half that ended a wait also sent its IPI: the target took
// it (it may still be pending while the target runs masked, so `serve` opens
// interrupt windows meanwhile), or the sender sent none because the target
// was between two waits then, which is no lost wake (a waiter checks for
// pending lines after it publishes itself). The caller then tries again.
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
            50'000'000))
        return WakeIpi::received;
    require(sender.ipis_sent.load(std::memory_order_acquire) == sent, "a wake IPI was sent but never taken");
    return WakeIpi::not_sent;
}
constexpr usize wake_attempts = 8;

// The interrupt service on the I/O APIC the MADT describes: the 8259 pair and
// LINT0 masked, the PIT on its GSI to the boot CPU, the COM1 line delivered to
// whichever CPU it is routed to. With a secondary CPU online the line reaches
// it in ring 3 and in its idle wait, and a top half on the boot CPU ends the
// secondary's device wait with an IPI.
void check_device_interrupts()
{
    const bool secondary = secondary_phase.load(std::memory_order_acquire) == secondary_done;
    ok::smp::detail::ipi_wake = []() noexcept { ok_interrupt_wake(); };
    require(interrupt::start_platform_irqs().ok() && ioapic::active(), "interrupt service on the I/O APIC");
    const auto line = interrupt::isa_irq_line(uart_irq, interrupt::IrqTrigger::edge);
    require(line.ok() && line.value().line == ioapic::firmware.isa_irq(uart_irq).gsi, "COM1 GSI from the MADT");
    uart_line = line.value().line;
    require(interrupt::bind_platform_irq(uart_line, line.value().trigger, uart_service, nullptr).ok() &&
                interrupt::platform_irq_target(uart_line) == 0,
            "bind the COM1 line");

    // The boot CPU halts in its interrupt wait and takes the line there.
    raise_uart_line();
    while (!interrupt::platform_irq_pending())
        require(interrupt::wait_for_interrupt(), "boot CPU interrupt wait");
    require(delivered_on(0) == 1 && interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == 1,
            "the boot CPU's wait runs the I/O APIC line's top half");

    // The PIT now preempts user code through its GSI.
    const native::PageMapping mappings[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_a), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_a), native::page_read | native::page_write},
    };
    auto boot_result = native::create_space(mappings);
    require(boot_result.ok(), "create the I/O APIC test space");
    auto boot_space = boot_result.value();
    auto frame = native::initial_frame(code_va, native::user_end);
    native::Trap tick{};
    for (usize attempt = 0; attempt < 16 && tick.kind != native::TrapKind::timer; ++attempt)
    {
        const auto next = native::enter(boot_space, frame);
        require(next.ok(), "enter for the PIT check");
        tick = next.value();
    }
    require(tick.kind == native::TrapKind::timer && tick.vector == ioapic::vector_base + ioapic::tick_gsi &&
                ioapic::tick_gsi == ioapic::firmware.isa_irq(0).gsi,
            "the PIT preempts user code on its GSI");
    require(native::destroy_space(boot_space).ok(), "release the I/O APIC test space");
    print("OK_NATIVE_IOAPIC scope=hardware boot_wait=pass pit_gsi=pass uart_gsi=pass\n");
    if (!secondary)
        return;

    const native::PageMapping secondary_mappings[] = {
        {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute},
        {data_va, reinterpret_cast<uptr>(data_c), native::page_read | native::page_write},
        {stack_va, reinterpret_cast<uptr>(stack_c), native::page_read | native::page_write},
    };
    auto secondary_result_space = native::create_space(secondary_mappings);
    require(secondary_result_space.ok(), "create the secondary device space");
    device_space = secondary_result_space.value();
    auto &boot = ok::smp::cpu_record(0);
    auto &record = ok::smp::cpu_record(1);

    // CPU 1's user trap takes the line routed to it and runs the top half,
    // whose IPI ends the boot CPU's interrupt wait; the boot CPU drains.
    require(interrupt::retarget_platform_irq(uart_line, 1).ok() && interrupt::platform_irq_target(uart_line) == 1,
            "route the COM1 line to CPU 1");
    device_phase.store(device_user, std::memory_order_release);
    u32 serviced = 1;
    for (usize attempt = 1;; ++attempt)
    {
        const auto boot_ipis = boot.ipis_received.load();
        const auto sent = record.ipis_sent.load();
        device_raise.store(true, std::memory_order_release);
        while (!interrupt::platform_irq_pending())
            require(interrupt::wait_for_interrupt(), "boot CPU interrupt wait");
        require(wait_for([&] { return device_traps.load() == attempt; }, 2'000'000'000) &&
                    delivered_on(1) == attempt && delivered_on(0) == 1,
                "CPU 1's user trap runs the top half");
        const auto woken = wake_ipi(boot, boot_ipis, record, sent, [] { interrupt::serve_local_interrupts(); });
        require(interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == ++serviced &&
                    !interrupt::platform_irq_pending(),
                "the boot CPU drains CPU 1's line");
        if (woken == WakeIpi::received)
            break;
        require(attempt < wake_attempts, "the top half wakes the boot CPU with an IPI");
    }
    device_stop.store(true, std::memory_order_release);
    require(wait_for([] { return device_phase.load(std::memory_order_acquire) == device_user_done; },
                     2'000'000'000) &&
                device_other.load() == 0,
            "CPU 1 left ring 3");

    // CPU 1's idle wait takes the line and runs the top half.
    device_phase.store(device_idle, std::memory_order_release);
    const auto secondary_delivered = delivered_on(1);
    raise_uart_line();
    while (!interrupt::platform_irq_pending())
        require(interrupt::wait_for_interrupt(), "boot CPU interrupt wait");
    require(delivered_on(1) == secondary_delivered + 1 && delivered_on(0) == 1 &&
                interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == ++serviced,
            "CPU 1's idle wait runs the top half");

    // CPU 1 waits for a device the way a driver inside a syscall does. The
    // line now goes to the boot CPU, which takes it with interrupts masked
    // (as a CPU spinning for the kernel lock does); its top half ends CPU 1's
    // wait with an IPI.
    require(interrupt::retarget_platform_irq(uart_line, 0).ok() && interrupt::platform_irq_target(uart_line) == 0,
            "route the COM1 line back to the boot CPU");
    for (usize attempt = 1;; ++attempt)
    {
        const auto waits = interrupt::platform_irq_cpu_statistics(1).waits;
        const auto secondary_ipis = record.ipis_received.load();
        const auto sent = boot.ipis_sent.load();
        device_phase.store(device_wait, std::memory_order_release);
        require(wait_for([&] { return interrupt::platform_irq_cpu_statistics(1).waits != waits; }, 2'000'000'000),
                "CPU 1 halts in its device wait");
        raise_uart_line();
        require(wait_for(
                    [&] {
                        interrupt::serve_local_interrupts();
                        return delivered_on(0) == 1 + attempt;
                    },
                    2'000'000'000),
                "the boot CPU serves the line while spinning");
        require(wait_for([] { return device_phase.load(std::memory_order_acquire) == device_woken; },
                         2'000'000'000) &&
                    device_other.load() == 0,
                "the boot CPU's top half ends CPU 1's device wait");
        // CPU 1 takes an IPI still pending in its next wait_local().
        const auto woken = wake_ipi(record, secondary_ipis, boot, sent, [] {});
        require(interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == ++serviced &&
                    delivered_on(1) == secondary_delivered + 1,
                "drain after the secondary device wait");
        if (woken == WakeIpi::received)
            break;
        require(attempt < wake_attempts, "the boot CPU's top half ends CPU 1's device wait with an IPI");
    }
    print("OK_NATIVE_IOAPIC scope=hardware cpus=2 user_top_half=pass wake_ipi=pass idle_top_half=pass "
          "secondary_device_wait=pass spin_serve=pass\n");
}

void check_fault(native::AddressSpace &space, const u8 *entry, native::TrapKind kind, uptr address, uptr error_mask)
{
    auto frame = native::initial_frame(code_va + static_cast<uptr>(entry - ok_native_user_begin), native::user_end);
    const auto trap = next_non_timer(space, frame);
    require(trap.kind == kind, "unexpected CPU fault kind");
    require(trap.fault_address == address, "unexpected hardware CR2");
    require((trap.error_code & error_mask) == error_mask, "missing hardware page-fault error bits");
}
} // namespace

// The secondary's C++ entry after the trampoline and its per-CPU setup.
extern "C" [[noreturn]] void ok_smp_secondary_main(ok::smp::CpuId cpu)
{
    auto &record = ok::smp::cpu_record(cpu);
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

extern "C" [[noreturn]] void kernel_main()
{
    out(0x3f9, 0);
    out(0x3fb, 0x80);
    out(0x3f8, 3);
    out(0x3f9, 0);
    out(0x3fb, 3);
    out(0x3fa, 0xc7);
    out(0x3fc, 0x0b);
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
    require(!native::user_range(space_a, 0x100000, 1), "reject kernel pointer");
    require(!native::user_range(space_a, data_va + 4095, 2), "reject partially unmapped buffer");
    require(!native::user_range(space_a, native::user_end - 1, static_cast<usize>(-1)), "reject overflowed buffer");
    require(!native::user_range(space_a, code_va, 1, true), "reject write to code");
    const u8 marker = 0xa5;
    require(native::copy_to_user(space_a, data_va + 128, &marker, 1).ok() && data_a[128] == marker,
            "copy user data through safe physical alias");
    print("OK_NATIVE_TEST user_copy=pass\n");

    auto frame_a = native::initial_frame(code_va, native::user_end);
    uptr boot_root;
    asm volatile("mov %%cr3, %0" : "=r"(boot_root));
    auto trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall, "CPL3 SYSCALL trap");
    const auto call = native::syscall(frame_a);
    require(call.number == 0x1234 && call.arguments[0] == 0x23 && call.arguments[1] == 42,
            "actual CPL3 and syscall registers");
    require(frame_a.registers[13] == 0x1122334455667788ULL && frame_a.registers[3] == 0x8877665544332211ULL,
            "callee-saved user registers");
    require(frame_a.fp_state[160] == 0x5a, "SSE user state saved before kernel C++");
    print("OK_NATIVE_TEST cpl3=pass syscall=pass registers=pass\n");
    native::set_return_value(frame_a, 7);
    trap = next_non_timer(space_a, frame_a);
    require(trap.kind == native::TrapKind::syscall && native::syscall(frame_a).number == 0x2345,
            "syscall return resumes user instruction");
    require(data_a[0] == 7 && data_a[16] == 0x5a, "user return register and SSE state restored");

    auto frame_b = native::initial_frame(code_va, native::user_end);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall, "second process syscall");
    native::set_return_value(frame_b, 11);
    trap = next_non_timer(space_b, frame_b);
    require(trap.kind == native::TrapKind::syscall && data_b[0] == 11 && data_a[0] == 7,
            "same virtual address has distinct physical process storage");
    print("OK_NATIVE_TEST address_spaces=pass sse=pass\n");
    check_tag_churn(space_b, data_b);
    auto timer = native::enter(space_a, frame_a);
    require(timer.ok() && timer.value().kind == native::TrapKind::timer && timer.value().vector == 32,
            "hardware PIT preempts non-cooperative user loop");
    print("OK_NATIVE_TEST timer_preemption=pass\n");

    check_fault(space_a, ok_native_user_kernel_read, native::TrapKind::page_fault, 0x100000, 5);
    check_fault(space_a, ok_native_user_readonly_write, native::TrapKind::page_fault, code_va, 7);
    check_fault(space_a, ok_native_user_nx, native::TrapKind::page_fault, data_va, 21);
    check_fault(space_a, ok_native_user_unmapped, native::TrapKind::page_fault, 0x60000000, 4);
    check_fault(space_a, ok_native_user_invalid, native::TrapKind::exception, 0, 0);
    print("OK_NATIVE_TEST supervisor_isolation=pass readonly=pass nx=pass faults=pass\n");
    uptr restored_root;
    asm volatile("mov %%cr3, %0" : "=r"(restored_root));
    require(restored_root == boot_root, "restore kernel CR3 after user fault");
    require(native::destroy_space(space_a).ok() && native::destroy_space(space_b).ok(),
            "release native address spaces");
    require(!native::user_range(space_a, data_va, 1), "stale address-space handle rejected");
    dense_mappings[0] = {code_va, reinterpret_cast<uptr>(code), native::page_read | native::page_execute};
    for (usize i = 0; i < dense_pages; ++i)
        dense_mappings[i + 1] = {dense_va + i * 4096, reinterpret_cast<uptr>(dense_data[i]),
                                 native::page_read | native::page_write};
    dense_mappings[dense_pages + 1] =
        {stack_va, reinterpret_cast<uptr>(stack_a), native::page_read | native::page_write};
    auto dense_result = native::create_space(dense_mappings);
    require(dense_result.ok(), "create more than 128 hardware user mappings");
    auto dense_space = dense_result.value();
    auto dense_frame = native::initial_frame(code_va + static_cast<uptr>(ok_native_user_dense - ok_native_user_begin),
                                            native::user_end);
    trap = next_non_timer(dense_space, dense_frame);
    require(trap.kind == native::TrapKind::syscall && native::syscall(dense_frame).number == 0x3456,
            "ring-3 loop touched 192 pages");
    for (usize i = 0; i < dense_pages; ++i)
        require(dense_data[i][0] == static_cast<u8>(dense_pages - i) && dense_data[i][1] == 0,
                "ring-3 page bytes and zero fill");
    print("OK_NATIVE_DENSE pages=192 touch=pass zero=pass\n");

    require(native::destroy_space(dense_space).ok(), "retire dense table before protection change");
    dense_mappings[101].flags = 0;
    dense_result = native::create_space(dense_mappings);
    require(dense_result.ok(), "create PROT_NONE dense table");
    dense_space = dense_result.value();
    require(!native::user_range(dense_space, dense_va + 100 * 4096, 1) &&
            dense_data[100][0] == static_cast<u8>(dense_pages - 100), "PROT_NONE has no user translation");
    require(native::destroy_space(dense_space).ok(), "retire protected table");
    dense_mappings[101].flags = native::page_read | native::page_write;
    for (usize src = 0, dst = 0; src < dense_mappings.size(); ++src)
        if (src != 101) sparse_mappings[dst++] = dense_mappings[src];
    auto sparse_result = native::create_space(sparse_mappings);
    require(sparse_result.ok(), "create sparse dense table");
    auto sparse_space = sparse_result.value();
    require(!native::user_range(sparse_space, dense_va + 100 * 4096, 1), "unmapped page has no user translation");
    require(native::destroy_space(sparse_space).ok(), "retire sparse table");
    dense_mappings[101].physical_address = reinterpret_cast<uptr>(dense_replacement);
    dense_result = native::create_space(dense_mappings);
    require(dense_result.ok(), "remap zeroed replacement page");
    dense_space = dense_result.value();
    require(native::user_range(dense_space, dense_va + 100 * 4096, 1, true) && dense_replacement[0] == 0,
            "remap restores writable zero page");
    std::array<native::PageMapping, 17> scattered{};
    for (usize i = 0; i < scattered.size(); ++i)
        scattered[i] = {native::user_begin + i * 0x200000, reinterpret_cast<uptr>(dense_data[i]),
                        native::page_read};
    // Translation tables come from runtime storage; a budget that runs out
    // partway through the 17 leaf tables makes growth fail.
    const usize tables_before = native::probe::table_pages_used;
    native::probe::table_page_budget = tables_before + 4;
    require(!native::create_space(scattered) && native::probe::table_pages_used == tables_before &&
                native::user_range(dense_space, dense_va, 1) && dense_data[0][0] == static_cast<u8>(dense_pages),
            "failed table growth releases its tables and preserves old bytes");
    native::probe::table_page_budget = native::probe::table_arena_pages;
    require(native::destroy_space(dense_space).ok(), "retire remapped dense table");
    print("OK_NATIVE_DENSE none=pass unmap=pass remap=pass failure=pass\n");
    check_secondary_cpu();
    check_device_interrupts();
    print("OK_NATIVE_TEST_PASS scope=hardware architecture=x86_64\n");
    finish(true);
}
