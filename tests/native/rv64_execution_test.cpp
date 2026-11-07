#include "ok/arch/execution.hpp"
#include "ok/arch/smp.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/smp/bringup.hpp"
#include "execution_table_pages.hpp"
#include "../../src/arch/riscv_common/clint.hpp"
#include "../../src/arch/tlb_tags.hpp"
namespace hw = ok::arch::execution;
using namespace ok;
extern "C" const std::byte ok_rv64_probe_begin[], ok_rv64_probe_timer[], ok_rv64_probe_write_fault[],
    ok_rv64_probe_kernel_fault[], ok_rv64_probe_float[], ok_rv64_probe_memory[], ok_rv64_probe_end[];
namespace
{
alignas(4096) std::array<std::byte, 4096> code{}, stack{}, other_stack{};
void emit(const char *text)
{
    auto *uart = reinterpret_cast<volatile u8 *>(0x10000000);
    while (*text)
        *uart = static_cast<u8>(*text++);
}
[[noreturn]] void end(bool success)
{
    *reinterpret_cast<volatile u32 *>(0x100000) = success ? 0x5555u : 0x3333u;
    for (;;)
        asm volatile("wfi");
}
void check(bool success, const char *message)
{
    if (!success)
    {
        emit("RV64_NATIVE_FAIL ");
        emit(message);
        emit("\n");
        end(false);
    }
}
uptr entry(const std::byte *label)
{
    return hw::user_begin + static_cast<uptr>(label - ok_rv64_probe_begin);
}
// Bounded on the CLINT's 10 MHz mtime, so a hart that never answers fails.
template <typename Done> bool wait_for(Done done, u64 ticks = 20'000'000)
{
    const u64 start = arch::riscv::Clint::time();
    while (!done())
        if (arch::riscv::Clint::time() - start > ticks)
            return done();
    return true;
}

// One pass of the memory probe: it adds one to the word at user_begin+0x1000
// and reports the sum.
u64 count_through_user(hw::AddressSpace &space)
{
    auto frame = hw::initial_frame(entry(ok_rv64_probe_memory), hw::user_begin + 0x2000 - 16);
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        const auto trap = hw::enter(space, frame);
        check(trap.ok(), "tag churn entry");
        if (trap.value().kind == hw::TrapKind::syscall)
            return frame.registers[10];
        check(trap.value().kind == hw::TrapKind::timer, "tag churn trap");
    }
    check(false, "tag churn made no progress");
    return 0;
}

// More spaces than this hart has ASID slots, created and retired in turn like
// mmap churn: tags roll over and are reused, each space counts in its own
// frame (three frames rotate), and the next entry after a remap counts in the
// new frame. This runs the tagged entry path, sfence.vma by ASID and the
// rollover; it cannot catch a missing invalidation under QEMU TCG, which
// drops its whole TLB on every satp write. Stale-entry detection is the hosted
// model's (scripts/test_tlb_tags.py).
alignas(4096) std::array<std::array<u64, 512>, 3> churn_data{};
alignas(4096) std::array<u64, 512> kept_data{}, remap_target{};
void check_tag_churn()
{
    constexpr usize spaces = 2 * arch::detail::tlb_tag_slots + 5;
    const auto before = hw::tlb_tag_report(1);
    // A space keeps viewing its mapping records; the runtime rewrites them
    // with a remap, as this does.
    static std::array<hw::PageMapping, 2> kept_pages{
        {{hw::user_begin, reinterpret_cast<uptr>(code.data()), hw::page_read | hw::page_execute},
         {hw::user_begin + 0x1000, reinterpret_cast<uptr>(kept_data.data()), hw::page_read | hw::page_write}}};
    auto kept_result = hw::create_space(kept_pages);
    check(kept_result.ok(), "create long-lived space");
    auto kept = kept_result.value();
    u64 kept_count = 0;
    for (usize i = 0; i < spaces; ++i)
    {
        auto &data = churn_data[i % 3];
        data[0] = 1000 * (i + 1);
        auto pages = kept_pages;
        pages[1].physical_address = reinterpret_cast<uptr>(data.data());
        auto created = hw::create_space(pages);
        check(created.ok(), "create tag churn space");
        auto space = created.value();
        check(count_through_user(space) == 1000 * (i + 1) + 1 && data[0] == 1000 * (i + 1) + 1,
              "tag churn space counts in its own frame");
        check(hw::destroy_space(space).ok(), "retire tag churn space");
        if (i % 7 == 0)
            check(count_through_user(kept) == ++kept_count && kept_data[0] == kept_count,
                  "long-lived space keeps its frame across rollovers");
    }
    remap_target[0] = 500;
    kept_pages[1].physical_address = reinterpret_cast<uptr>(remap_target.data());
    const auto before_remap = hw::tlb_tag_report(1);
    check(hw::remap(kept, kept_pages[1]).ok(), "remap long-lived page");
    check(count_through_user(kept) == 501 && remap_target[0] == 501 && kept_data[0] == kept_count,
          "next entry after a remap uses the new frame");
    // The first entry after the remap fences the space's ASID (its generation
    // moved); later entries keep it.
    check(count_through_user(kept) == 502 &&
              hw::tlb_tag_report(1).invalidations == before_remap.invalidations + (before_remap.asid_bits ? 1 : 0),
          "one ASID invalidation for the remap");
    check(hw::destroy_space(kept).ok(), "retire long-lived space");
    const auto after = hw::tlb_tag_report(1);
    check(after.supported && !after.pcid &&
              (after.asid_bits == 0 ? after.rollovers == 0 : after.rollovers >= before.rollovers + 2),
          "ASID tags rolled over, or every entry flushed");
    if (after.asid_bits == 16)
        emit("RV64_NATIVE tlb_asid=16 churn=pass rollover=pass remap=pass\n");
    else if (after.asid_bits == 0)
        emit("RV64_NATIVE tlb_asid=none churn=pass remap=pass\n");
    else
        emit("RV64_NATIVE tlb_asid=narrow churn=pass rollover=pass remap=pass\n");
}

// The 16550's transmitter-empty interrupt (PLIC source 10) is a device line
// the probe raises at will: enabling it in IER asserts the line while THR is
// empty, and the bottom half disables it again.
constexpr interrupt::IrqLine uart_line = 10;
std::atomic<u32> uart_serviced{};
void raise_uart_line()
{
    *reinterpret_cast<volatile u8 *>(0x10000001) = 0x02;
}
void uart_service(void *, interrupt::IrqLine)
{
    *reinterpret_cast<volatile u8 *>(0x10000001) = 0;
    uart_serviced.fetch_add(1, std::memory_order_relaxed);
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
WakeIpi wake_ipi(const smp::CpuRecord &target, uptr received, const smp::CpuRecord &sender, uptr sent, Serve serve)
{
    if (wait_for(
            [&] {
                serve();
                return target.ipis_received.load(std::memory_order_acquire) != received;
            },
            500'000))
        return WakeIpi::received;
    check(sender.ipis_sent.load(std::memory_order_acquire) == sent, "a wake IPI was sent but never taken");
    return WakeIpi::not_sent;
}
constexpr usize wake_attempts = 8;

// Hart 1's share: one pass of the memory probe in its own space, then the
// counting loop until hart 0 asks it to stop, reporting how each entry ended
// (asked to, it raises the UART line from its kernel side), then one more
// memory pass. Hart 0 remapped the counted page meanwhile, so that last pass
// must count in the new frame: the path of a hart that cached the old
// translation (under its own ASID, where it has them) and left. Stopped, it
// idles; in phase 4 it waits for a device interrupt as a driver inside a
// syscall would; in phase 6 it raises the line routed to it and, with the
// source pending at its PLIC context, retargets its lines away as before
// going offline.
alignas(16) std::array<std::byte, 16384> hart_one_stack{};
alignas(4096) std::array<u64, 512> hart_one_target{};
hw::AddressSpace hart_one_space{};
// 1 run, 2 stop, 3 stopped, 4 device wait, 5 woken, 6 retire, 7 retired
std::atomic<u32> hart_one_phase{};
std::atomic<u32> hart_one_timers{}, hart_one_ipis{}, hart_one_devices{}, hart_one_other{};
std::atomic<uptr> hart_one_count{}, hart_one_before{}, hart_one_after{}, hart_one_moved{};
std::atomic<bool> hart_one_identity{}, hart_one_raise{}, hart_one_accepted{};
// One memory pass on hart 1, inside its user window: the sum it reports, or 0
// when an entry ended otherwise.
u64 hart_one_memory_pass()
{
    auto frame = hw::initial_frame(entry(ok_rv64_probe_memory), hw::user_begin + 0x2000 - 16);
    for (usize attempt = 0; attempt < 128; ++attempt)
    {
        smp::begin_user(hart_one_space.root);
        const auto trap = hw::enter(hart_one_space, frame);
        smp::end_user();
        if (!trap)
            return 0;
        if (trap.value().kind == hw::TrapKind::syscall)
            return frame.registers[10];
        if (trap.value().kind != hw::TrapKind::timer && trap.value().kind != hw::TrapKind::interrupt)
            return 0;
    }
    return 0;
}
} // namespace
extern "C" [[noreturn]] void ok_smp_secondary_main(smp::CpuId cpu)
{
    auto &record = smp::cpu_record(cpu);
    hart_one_identity.store(smp::current_cpu() == cpu, std::memory_order_relaxed);
    record.reported_id.store(static_cast<uptr>(arch::smp::current_hardware_id()), std::memory_order_release);
    record.state.store(static_cast<u8>(smp::CpuState::online), std::memory_order_release);
    while (hart_one_phase.load(std::memory_order_acquire) == 0)
        arch::ArchOperations::spin_pause();
    hart_one_before.store(hart_one_memory_pass(), std::memory_order_release);
    auto frame = hw::initial_frame(entry(ok_rv64_probe_timer), hw::user_begin + 0x2000 - 16);
    while (hart_one_phase.load(std::memory_order_acquire) == 1)
    {
        smp::begin_user(hart_one_space.root);
        const auto trap = hw::enter(hart_one_space, frame);
        smp::end_user();
        if (trap && trap.value().kind == hw::TrapKind::timer)
            hart_one_timers.fetch_add(1, std::memory_order_relaxed);
        else if (trap && trap.value().kind == hw::TrapKind::interrupt && trap.value().vector == 3)
            hart_one_ipis.fetch_add(1, std::memory_order_relaxed);
        else if (trap && trap.value().kind == hw::TrapKind::interrupt && trap.value().vector == uart_line)
            hart_one_devices.fetch_add(1, std::memory_order_relaxed);
        else
            hart_one_other.fetch_add(1, std::memory_order_relaxed);
        if (hart_one_raise.exchange(false, std::memory_order_acq_rel))
        {
            // Long enough for hart 0 to halt in its wait first.
            const u64 start = arch::riscv::Clint::time();
            while (arch::riscv::Clint::time() - start < 20'000)
                arch::ArchOperations::spin_pause();
            raise_uart_line();
        }
    }
    hart_one_count.store(frame.registers[8], std::memory_order_relaxed);
    hart_one_after.store(hart_one_memory_pass(), std::memory_order_relaxed);
    hart_one_phase.store(3, std::memory_order_release);
    for (;;)
    {
        const auto phase = hart_one_phase.load(std::memory_order_acquire);
        if (phase == 4)
        {
            while (!interrupt::platform_irq_pending())
                check(interrupt::wait_for_interrupt(), "a secondary hart waits for a device line");
            hart_one_phase.store(5, std::memory_order_release);
        }
        else if (phase == 6)
        {
            raise_uart_line();
            hart_one_accepted.store(wait_for([] {
                uptr pending;
                asm volatile("csrr %0, mip" : "=r"(pending));
                return (pending & arch::riscv::mie_meie) != 0;
            }));
            hart_one_moved.store(interrupt::retarget_platform_irqs_away(1));
            hart_one_phase.store(7, std::memory_order_release);
        }
        else
            arch::smp::wait_local();
    }
}
extern "C" void *memcpy(void *to, const void *from, usize size)
{
    auto *out = static_cast<volatile std::byte *>(to);
    const auto *in = static_cast<const std::byte *>(from);
    for (usize i = 0; i < size; ++i)
        out[i] = in[i];
    return to;
}
extern "C" void *memset(void *to, int value, usize size)
{
    auto *out = static_cast<volatile u8 *>(to);
    for (usize i = 0; i < size; ++i)
        out[i] = static_cast<u8>(value);
    return to;
}
// The DTB parser's string compares.
extern "C" int memcmp(const void *left, const void *right, usize size)
{
    const auto *a = static_cast<const volatile u8 *>(left), *b = static_cast<const volatile u8 *>(right);
    for (usize i = 0; i < size; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}
extern "C" void *memchr(const void *from, int value, usize size)
{
    const auto *in = static_cast<const volatile u8 *>(from);
    for (usize i = 0; i < size; ++i)
        if (in[i] == static_cast<u8>(value))
            return const_cast<u8 *>(const_cast<const u8 *>(in + i));
    return nullptr;
}
extern "C" [[noreturn]] void kernel_main()
{
    emit("RV64_NATIVE_BEGIN\n");
    check(hw::initialize().ok(), "initialize");
    for (usize i = 0; i < static_cast<usize>(ok_rv64_probe_end - ok_rv64_probe_begin); ++i)
        code[i] = ok_rv64_probe_begin[i];
    const std::array<hw::PageMapping, 2> pages{
        {{hw::user_begin, reinterpret_cast<uptr>(code.data()), hw::page_read | hw::page_execute},
         {hw::user_begin + 0x1000, reinterpret_cast<uptr>(stack.data()), hw::page_read | hw::page_write}}};
    auto result = hw::create_space(pages);
    check(result.ok(), "create space");
    auto space = result.value();
    auto frame = hw::initial_frame(hw::user_begin, hw::user_begin + 0x2000 - 16);
    auto trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 65 &&
              hw::syscall(frame).arguments[0] == 42 && frame.registers[3] == 0x12345 && frame.registers[4] == 0x67890,
          "U ecall");
    hw::set_return_value(frame, 81);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 66 &&
              hw::syscall(frame).arguments[0] == 82 && frame.registers[3] == 0x12345 && frame.registers[4] == 0x67890,
          "syscall return");
    emit("RV64_NATIVE syscall=pass return=pass\n");
    frame = hw::initial_frame(entry(ok_rv64_probe_timer), hw::user_begin + 0x2000 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[8] != 0, "timer");
    const auto count = frame.registers[8];
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::timer && frame.registers[8] > count, "timer resume");
    emit("RV64_NATIVE timer=pass context=pass\n");
    frame = hw::initial_frame(entry(ok_rv64_probe_write_fault), hw::user_begin + 0x2000 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 15, "read-only protection");
    frame = hw::initial_frame(entry(ok_rv64_probe_kernel_fault), hw::user_begin + 0x2000 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::page_fault && trap.value().vector == 13 &&
              trap.value().fault_address == 0x80000000,
          "kernel isolation");
    uptr kernel_status = 0;
    asm volatile("csrr %0, mstatus" : "=r"(kernel_status));
    const uptr fp_enabled = kernel_status | 0x6000;
    asm volatile("csrw mstatus, %0" ::"r"(fp_enabled) : "memory");
    asm volatile("fmv.d.x fs0, %0" ::"r"(uptr{0xabcdef}) : "memory");
    frame = hw::initial_frame(entry(ok_rv64_probe_float), hw::user_begin + 0x2000 - 16);
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).number == 67, "FP store");
    trap = hw::enter(space, frame);
    check(trap && trap.value().kind == hw::TrapKind::syscall && hw::syscall(frame).arguments[0] == 123, "FP restore");
    uptr kernel_fp = 0;
    asm volatile("fmv.x.d %0, fs0" : "=r"(kernel_fp));
    check(kernel_fp == 0xabcdef, "kernel callee-saved FP");
    asm volatile("csrw mstatus, %0" ::"r"(kernel_status) : "memory");
    emit("RV64_NATIVE protection=pass fp=pass\n");
    auto other_pages = pages;
    other_pages[1].physical_address = reinterpret_cast<uptr>(other_stack.data());
    auto second = hw::create_space(other_pages);
    check(second.ok(), "second space");
    const u64 value = 0x12345678;
    u64 copied = 1;
    check(hw::copy_to_user(space, hw::user_begin + 0x1000, &value, sizeof(value)).ok(), "copy to user");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 0x1000, sizeof(copied)).ok() && copied == 0,
          "space isolation");
    auto memory_a = hw::initial_frame(entry(ok_rv64_probe_memory), hw::user_begin + 0x2000 - 16);
    auto memory_b = hw::initial_frame(entry(ok_rv64_probe_memory), hw::user_begin + 0x2000 - 16);
    trap = hw::enter(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[10] == value + 1,
          "first space U load/store");
    trap = hw::enter(second.value(), memory_b);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_b.registers[10] == 1,
          "second space U load/store");
    trap = hw::enter(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[10] == value + 2,
          "SATP switch back");
    check(hw::copy_from_user(second.value(), &copied, hw::user_begin + 0x1000, sizeof(copied)).ok() && copied == 1,
          "second space survived other user writes");
    emit("RV64_NATIVE address_spaces=pass\n");
    check_tag_churn();
    // Hart 1 from the DTB, released through its start mailbox and MSIP, runs
    // user code in the second space while this hart runs its own; remapping
    // that space first sends hart 1 out of U-mode with an IPI.
    boot::CpuTopologyMap harts{};
    check(arch::smp::discover(harts).ok() && harts.count == 2 && harts.method == boot::CpuStartMethod::riscv_msip &&
              harts.boot_known && harts.cpus[harts.boot_index].hardware_id == 0,
          "hart discovery");
    check(arch::smp::prepare_boot_cpu(harts).ok(), "boot hart IPI setup");
    smp::detail::ipi_sender = &arch::smp::send_ipi;
    auto &hart_one = smp::cpu_record(1);
    hart_one.hardware_id = 1;
    hart_one_space = second.value();
    const auto stack_lower = reinterpret_cast<uptr>(hart_one_stack.data());
    check(arch::smp::start(1, 1, stack_lower, stack_lower + hart_one_stack.size()).ok(), "hart start request");
    check(wait_for([&] {
              return hart_one.state.load(std::memory_order_acquire) == static_cast<u8>(smp::CpuState::online);
          }) && hart_one.reported_id.load(std::memory_order_acquire) == 1 &&
              hart_one_identity.load(std::memory_order_relaxed),
          "hart 1 runs kernel code with its own block");
    smp::detail::online.store(0b11, std::memory_order_release);
    hart_one_phase.store(1, std::memory_order_release);
    check(wait_for([&] { return hart_one.local_ticks.load(std::memory_order_relaxed) >= 2; }) &&
              hart_one_before.load(std::memory_order_acquire) == 2,
          "hart 1 local timer and memory pass");
    const auto boot_tags = hw::tlb_tag_report(1);
    const auto hart_one_tags = hw::tlb_tag_report(2);
    check(hart_one_tags.supported && hart_one_tags.asid_bits == boot_tags.asid_bits &&
              (hart_one_tags.invalidations != 0) == (hart_one_tags.asid_bits != 0),
          "hart 1 probed its own ASIDLEN");
    trap = hw::enter(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[10] == value + 3,
          "user entry beside hart 1");
    // The shootdown waits only for an open user window, which hart 1 leaves
    // for microseconds per 10 ms quantum; retry until one was waited for.
    hart_one_target[0] = 700;
    other_pages[1].physical_address = reinterpret_cast<uptr>(hart_one_target.data());
    for (usize attempt = 0; attempt < 100 && smp::cpu_record(0).shootdowns.load() == 0; ++attempt)
        check(hw::remap(second.value(), other_pages[1]).ok(), "remap beside hart 1");
    check(smp::cpu_record(0).shootdowns.load() != 0 &&
              wait_for([&] { return hart_one_ipis.load(std::memory_order_relaxed) != 0; }),
          "shootdown IPI takes hart 1 out of U-mode");
    // A device line routed to hart 1: its user trap claims the line through
    // hart 1's PLIC context and runs the top half there, which ends hart 0's
    // interrupt wait with an IPI; hart 0 runs the bottom half.
    smp::detail::ipi_wake = []() noexcept { ok_interrupt_wake(); };
    check(interrupt::start_platform_irqs().ok(), "interrupt service");
    check(interrupt::bind_platform_irq(uart_line, interrupt::IrqTrigger::level, uart_service, nullptr).ok() &&
              interrupt::retarget_platform_irq(uart_line, 1).ok() && interrupt::platform_irq_target(uart_line) == 1,
          "route the UART line to hart 1");
    u32 serviced = 0;
    for (u32 attempt = 1;; ++attempt)
    {
        const auto boot_ipis = smp::cpu_record(0).ipis_received.load();
        const auto sent = hart_one.ipis_sent.load();
        hart_one_raise.store(true, std::memory_order_release);
        while (!interrupt::platform_irq_pending())
            check(interrupt::wait_for_interrupt(), "boot hart interrupt wait");
        check(wait_for([&] { return hart_one_devices.load() == attempt; }) &&
                  interrupt::platform_irq_cpu_statistics(1).delivered == attempt &&
                  interrupt::platform_irq_cpu_statistics(0).delivered == 0,
              "hart 1's user trap runs the top half");
        const auto woken =
            wake_ipi(smp::cpu_record(0), boot_ipis, hart_one, sent, [] { interrupt::serve_local_interrupts(); });
        check(interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == ++serviced &&
                  !interrupt::platform_irq_pending(),
              "hart 0 drains hart 1's line");
        if (woken == WakeIpi::received)
            break;
        check(attempt < wake_attempts, "the top half wakes hart 0 with an IPI");
    }
    emit("RV64_NATIVE device_route=hart1 user_top_half=pass wake_ipi=pass\n");
    hart_one_phase.store(2, std::memory_order_release);
    check(wait_for([&] { return hart_one_phase.load(std::memory_order_acquire) == 3; }) &&
              hart_one_timers.load() >= 2 && hart_one_other.load() == 0 && hart_one_count.load() != 0 &&
              smp::cpu_record(0).local_ticks.load() == 0,
          "hart 1 user traps");
    check(hart_one_after.load(std::memory_order_relaxed) == 701 && hart_one_target[0] == 701 &&
              reinterpret_cast<const u64 *>(other_stack.data())[0] == 2,
          "hart 1's next entry after the remap counts in the new frame");
    const auto hart_one_after_tags = hw::tlb_tag_report(2);
    check(hart_one_after_tags.asid_bits != 0 ? hart_one_after_tags.invalidations > hart_one_tags.invalidations
                                             : hart_one_after_tags.invalidations == 0,
          "hart 1 invalidated its ASID for the moved generation");
    const auto received = hart_one.ipis_received.load(std::memory_order_acquire);
    smp::send_ipi(1, smp::ipi_ping);
    check(wait_for([&] { return hart_one.ipis_received.load(std::memory_order_acquire) != received; }),
          "IPI wakes hart 1's idle wait");
    emit("RV64_NATIVE harts=2 start=pass local_timer=pass shootdown=pass remote_remap=pass idle_ipi=pass\n");
    // Hart 1's idle wait claims the line routed to it and runs the top half.
    const auto hart_one_delivered = interrupt::platform_irq_cpu_statistics(1).delivered;
    raise_uart_line();
    while (!interrupt::platform_irq_pending())
        check(interrupt::wait_for_interrupt(), "boot hart interrupt wait");
    check(interrupt::platform_irq_cpu_statistics(1).delivered == hart_one_delivered + 1 &&
              interrupt::platform_irq_cpu_statistics(0).delivered == 0 && interrupt::drain_platform_irqs() == 1 &&
              uart_serviced.load() == ++serviced,
          "hart 1's idle wait runs the top half");
    // Hart 1 waits for a device the way a driver inside a syscall does. The
    // line now goes to hart 0, which takes it with interrupts masked (as a
    // CPU spinning for the kernel lock does); its top half ends hart 1's wait
    // with an IPI.
    check(interrupt::retarget_platform_irq(uart_line, 0).ok() && interrupt::platform_irq_target(uart_line) == 0,
          "route the UART line to hart 0");
    for (u32 attempt = 1;; ++attempt)
    {
        const auto waits = interrupt::platform_irq_cpu_statistics(1).waits;
        const auto secondary_ipis = hart_one.ipis_received.load();
        const auto sent = smp::cpu_record(0).ipis_sent.load();
        hart_one_phase.store(4, std::memory_order_release);
        check(wait_for([&] { return interrupt::platform_irq_cpu_statistics(1).waits != waits; }),
              "hart 1 halts in its device wait");
        raise_uart_line();
        check(wait_for([&] {
                  interrupt::serve_local_interrupts();
                  return interrupt::platform_irq_cpu_statistics(0).delivered == attempt;
              }),
              "hart 0 serves the line while spinning");
        check(wait_for([&] { return hart_one_phase.load(std::memory_order_acquire) == 5; }),
              "hart 0's top half ends hart 1's device wait");
        // Hart 1 takes an IPI still pending in its next wait_local().
        const auto woken = wake_ipi(hart_one, secondary_ipis, smp::cpu_record(0), sent, [] {});
        check(interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == ++serviced &&
                  interrupt::platform_irq_cpu_statistics(1).delivered == hart_one_delivered + 1,
              "drain after the secondary device wait");
        if (woken == WakeIpi::received)
            break;
        check(attempt < wake_attempts, "hart 0's top half ends hart 1's device wait with an IPI");
    }
    emit("RV64_NATIVE idle_top_half=pass secondary_device_wait=pass spin_serve=pass\n");
    // Hart 1 leaves as a hotplug path would. The source pending, unclaimed,
    // at its PLIC context is not lost: it runs one top half, on hart 1
    // before retarget_platform_irqs_away() returns or on hart 0 once the
    // enable bit moved. The line then reaches hart 0 only.
    check(interrupt::retarget_platform_irq(uart_line, 1).ok(), "route the UART line to hart 1 again");
    const auto delivered_before = interrupt::platform_irq_cpu_statistics(0).delivered +
                                  interrupt::platform_irq_cpu_statistics(1).delivered;
    hart_one_phase.store(6, std::memory_order_release);
    check(wait_for([&] { return hart_one_phase.load(std::memory_order_acquire) == 7; }) &&
              hart_one_accepted.load() && hart_one_moved.load() == 1 &&
              interrupt::platform_irq_target(uart_line) == 0,
          "hart 1 retargets its line away");
    check(wait_for([&] {
              interrupt::serve_local_interrupts();
              return interrupt::platform_irq_pending();
          }) && interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == ++serviced &&
              interrupt::platform_irq_cpu_statistics(0).delivered +
                      interrupt::platform_irq_cpu_statistics(1).delivered ==
                  delivered_before + 1,
          "the source hart 1 held runs one top half");
    const auto boot_delivered = interrupt::platform_irq_cpu_statistics(0).delivered;
    const auto secondary_delivered = interrupt::platform_irq_cpu_statistics(1).delivered;
    raise_uart_line();
    check(wait_for([&] {
              interrupt::serve_local_interrupts();
              return interrupt::platform_irq_cpu_statistics(0).delivered == boot_delivered + 1;
          }) && interrupt::drain_platform_irqs() == 1 && uart_serviced.load() == ++serviced &&
              interrupt::platform_irq_cpu_statistics(1).delivered == secondary_delivered,
          "the line reaches hart 0 after the retarget");
    emit("RV64_NATIVE retarget_away=pass\n");
    check(!hw::copy_to_user(space, hw::user_begin, &value, sizeof(value)), "reject read-only copy");
    check(!hw::user_range(space, hw::user_begin + 0x1fff, 2, true), "reject unmapped boundary");
    check(hw::copy_from_user(space, nullptr, 0, 0).ok(), "empty null copy");
    check(!hw::copy_from_user(space, nullptr, hw::user_begin, 1), "null kernel copy target");
    auto invalid = pages;
    invalid[0].flags |= hw::page_write;
    check(!hw::create_space(invalid), "reject writable executable page");
    auto bad_frame = hw::initial_frame(hw::user_begin, hw::user_begin + 0x4000);
    // Like Linux, entry does not need a mapped stack; this probe reaches its
    // syscall without touching one.
    auto unmapped = hw::enter(space, bad_frame);
    while (unmapped && unmapped.value().kind == hw::TrapKind::timer) unmapped = hw::enter(space, bad_frame);
    check(unmapped && unmapped.value().kind == hw::TrapKind::syscall, "enter with an unmapped stack");
    const auto stale = space;
    check(hw::destroy_space(space).ok(), "destroy");
    check(!hw::user_range(stale, hw::user_begin, 1), "reject stale space");
    check(hw::destroy_space(second.value()).ok(), "second destroy");
    emit("RV64_NATIVE_PASS scope=hardware\n");
    end(true);
}
