#include "ok/arch/smp.hpp"

#include "cpu_local.hpp"
#include "lapic.hpp"
#include "ok/arch/clock.hpp"
#include "ok/boot/acpi.hpp"
#include "ok/smp/bringup.hpp"
#include "ok/smp/cpu.hpp"
#include "tsc_sync.hpp"

#include <atomic>
#include <cstddef>
#include <span>

extern "C"
{
    extern const ok::u8 ok_x86_smp_trampoline[];
    extern const ok::u8 ok_x86_smp_trampoline_parameters[];
    extern const ok::u8 ok_x86_smp_trampoline_end[];
    [[noreturn]] void ok_x86_smp_secondary_entry(ok::smp::CpuId cpu);
    void ok_platform_console_write_char(char value) __attribute__((weak));
}

namespace ok::arch::smp
{
namespace
{
namespace lapic = x86::lapic;
namespace tsc = x86::tsc;

// The boot loader's dead bounce buffer: the first MiB is never allocated, and
// the E820 table at 0x5000 ends below it.
constexpr uptr trampoline_base = 0x8000;
constexpr u64 identity_limit = 0x40000000;
constexpr u32 delivery_init_assert = 0xc500;
constexpr u32 delivery_init_deassert = 0x8500;
constexpr u32 delivery_startup = 0x0600;
constexpr u32 delivery_fixed = 0x4000;
constexpr u32 delivery_extint = 0x700;
constexpr u32 delivery_nmi = 0x400;
constexpr u32 apic_software_enable = 0x100;
constexpr u32 timer_divide_16 = 0x3;

// smp.S's parameter block.
struct TrampolineParameters
{
    u64 arrived;
    u64 cr0;
    u64 cr3;
    u64 cr4;
    u64 entry;
    u64 stack;
    u64 cpu;
};

u64 read_msr(u32 index)
{
    u32 low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return (u64{high} << 32) | low;
}
void write_msr(u32 index, u64 value)
{
    asm volatile("wrmsr" : : "c"(index), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32)) : "memory");
}
u64 read_tsc()
{
    u32 low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (u64{high} << 32) | low;
}
void outw(u16 port, u16 value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}
u8 inb(u16 port)
{
    u8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

// Busy-waits on the boot CPU's calibrated TSC.
void delay_ns(u64 nanoseconds)
{
    const u64 cycles = clock_frequency() / 1000 * nanoseconds / 1'000'000 + 1;
    const u64 start = read_tsc();
    while (read_tsc() - start < cycles)
        ArchOperations::spin_pause();
}

// Firmware tables are read through the boot identity map, which ends at 1 GiB.
std::span<const std::byte> identity_window(void *, u64 physical, usize size)
{
    if (physical == 0 || physical >= identity_limit || size > identity_limit - physical)
        return {};
    return {reinterpret_cast<const std::byte *>(static_cast<uptr>(physical)), size};
}

// QEMU's FW_CFG_NB_CPUS, or 0 without fw_cfg.
u16 fw_cfg_cpu_count()
{
    outw(0x510, 0x0000);
    if (inb(0x511) != 'Q' || inb(0x511) != 'E' || inb(0x511) != 'M' || inb(0x511) != 'U')
        return 0;
    outw(0x510, 0x0005);
    const u16 low = inb(0x511);
    return static_cast<u16>(low | (u16{inb(0x511)} << 8));
}

void send_icr(u64 apic_id, u32 command)
{
    while ((lapic::reg(lapic::icr_low) & lapic::delivery_pending) != 0)
        ArchOperations::spin_pause();
    lapic::reg(lapic::icr_high) = static_cast<u32>(apic_id) << 24;
    lapic::reg(lapic::icr_low) = command;
    while ((lapic::reg(lapic::icr_low) & lapic::delivery_pending) != 0)
        ArchOperations::spin_pause();
}

// Counts the divide-by-16 timer over 10 ms of TSC.
Status calibrate_timer()
{
    const u64 window = clock_frequency() / 100;
    if (window == 0)
        return Status::not_initialized("local APIC timer calibration needs the TSC clock");
    lapic::reg(lapic::timer_divide) = timer_divide_16;
    lapic::reg(lapic::lvt_timer) = lapic::lvt_masked | lapic::timer_vector;
    lapic::reg(lapic::timer_initial) = ~u32{0};
    const u64 start = read_tsc();
    while (read_tsc() - start < window)
        ArchOperations::spin_pause();
    const u32 remaining = lapic::reg(lapic::timer_current);
    lapic::reg(lapic::timer_initial) = 0;
    if (remaining == 0 || remaining == ~u32{0})
        return Status::io_error("local APIC timer did not count");
    lapic::timer_period = ~u32{0} - remaining;
    return Status::success();
}

// A secondary takes only its periodic local timer and IPIs.
void prepare_secondary_apic() noexcept
{
    lapic::reg(lapic::task_priority) = 0;
    lapic::reg(lapic::lvt_lint0) = lapic::lvt_masked | delivery_extint;
    lapic::reg(lapic::lvt_lint1) = lapic::lvt_masked | delivery_nmi;
    lapic::reg(lapic::lvt_error) = lapic::lvt_masked;
    lapic::reg(lapic::spurious) = apic_software_enable | lapic::spurious_vector;
    lapic::reg(lapic::timer_divide) = timer_divide_16;
    lapic::reg(lapic::lvt_timer) = lapic::timer_periodic | lapic::timer_vector;
    lapic::reg(lapic::timer_initial) = lapic::timer_period;
}

// TSC synchronization. Each starting CPU answers the boot CPU's commands over
// one channel before it enters ok_smp_secondary_main; the boot CPU measures
// the offset and corrects it (tsc_sync.hpp decides how). Starts are serial,
// so one channel serves every CPU in turn.
constexpr u32 msr_tsc_adjust = 0x3b;
constexpr u32 feature_tsc_adjust = 1; // CPUID.7.0:EBX[1]
constexpr u32 feature_invariant = 2;  // CPUID.80000007:EDX[8]
constexpr u64 sync_ready_timeout_ns = 1'000'000'000;
constexpr u64 sync_answer_timeout_ns = 100'000'000;
constexpr u64 sync_slack_ns = 100;

enum SyncOp : u32
{
    sync_sample = 1,
    sync_adjust = 2,
    sync_add_offset = 3,
    sync_finish = 4,
};

// The boot CPU writes the first line, the starting CPU the second, so each
// round moves one line each way.
struct TscChannel
{
    alignas(64) std::atomic<u32> target{}; // the CPU being synchronized, 0 for none
    std::atomic<u32> command{};            // sequence << 3 | SyncOp
    std::atomic<u64> argument{};
    alignas(64) std::atomic<u32> ready{};  // the target's id once it follows commands
    std::atomic<u32> features{};
    std::atomic<u32> answer{};             // answer_tag(sequence, cpu) | refused
    std::atomic<u64> reading{};
};
constinit TscChannel tsc_channel{};
static_assert(ok::smp::max_cpus <= 16);

// The answering CPU is part of the tag, so a CPU the boot CPU gave up on
// cannot answer for the next one.
constexpr u32 answer_tag(u32 sequence, ok::smp::CpuId cpu) noexcept
{
    return sequence << 5 | u32{cpu} << 1;
}
u32 boot_tsc_features{};
// The worst verdict over the CPUs synchronized so far, if any.
bool tsc_synchronized{};
tsc::Verdict tsc_verdict{};
#if defined(OK_X86_TSC_SKEW_TEST)
enum SkewInjected : u8
{
    skew_none,
    skew_tsc,      // the IA32_TSC write took
    skew_software, // the CPU ignored it; its clock offset carries the rewind
};
constinit std::atomic<u8> skew_injected{};
#endif

u32 tsc_features() noexcept
{
    const auto cpuid = [](u32 leaf, u32 &b, u32 &d) {
        u32 a = leaf, c = 0;
        asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
        return a;
    };
    u32 b, d, features = 0;
    if (cpuid(0, b, d) >= 7)
    {
        cpuid(7, b, d);
        features |= (b & 2) != 0 ? feature_tsc_adjust : 0;
    }
    if (cpuid(0x80000000, b, d) >= 0x80000007)
    {
        cpuid(0x80000007, b, d);
        features |= (d & (1u << 8)) != 0 ? feature_invariant : 0;
    }
    return features;
}

u64 cycles_for(u64 nanoseconds) noexcept
{
    return clock_frequency() / 1000 * nanoseconds / 1'000'000 + 1;
}

// The boot CPU's side of tsc::SyncLink.
class BootLink
{
  public:
    BootLink(ok::smp::CpuId cpu, bool can_adjust, u64 timeout) noexcept
        : cpu_(cpu), can_adjust_(can_adjust), timeout_(timeout)
    {
    }
    bool round(tsc::Round &round) noexcept
    {
        round.before = execution::x86::clock_counter();
        if (!post(sync_sample, 0))
            return false;
        round.after = execution::x86::clock_counter();
        round.remote = tsc_channel.reading.load(std::memory_order_relaxed);
        return true;
    }
    [[nodiscard]] bool can_adjust() const noexcept
    {
        return can_adjust_;
    }
    bool adjust(i64 delta) noexcept
    {
        return post(sync_adjust, static_cast<u64>(delta)) && !refused_;
    }
    bool add_offset(i64 offset) noexcept
    {
        return post(sync_add_offset, static_cast<u64>(offset)) && !refused_;
    }
    bool finish() noexcept
    {
        return post(sync_finish, 0);
    }

  private:
    // Waits for the answer on the raw TSC: the boot CPU's offset is zero.
    bool post(SyncOp op, u64 argument) noexcept
    {
        const u32 sequence = ++sequence_;
        tsc_channel.argument.store(argument, std::memory_order_relaxed);
        tsc_channel.command.store(sequence << 3 | op, std::memory_order_release);
        const u64 start = read_tsc();
        for (;;)
        {
            const u32 answer = tsc_channel.answer.load(std::memory_order_acquire);
            if ((answer & ~1u) == answer_tag(sequence, cpu_))
            {
                refused_ = (answer & 1) != 0;
                return true;
            }
            if (read_tsc() - start >= timeout_)
                return false;
            ArchOperations::spin_pause();
        }
    }

    ok::smp::CpuId cpu_{};
    bool can_adjust_{};
    bool refused_{};
    u64 timeout_{};
    u32 sequence_{};
};
static_assert(tsc::SyncLink<BootLink>);

void console(std::string_view text) noexcept
{
    if (ok_platform_console_write_char != nullptr)
        for (const char value : text)
            ok_platform_console_write_char(value);
}

void console_signed(i64 value) noexcept
{
    char digits[20];
    usize count = 0;
    u64 rest = tsc::magnitude(value);
    do
    {
        digits[count++] = static_cast<char>('0' + rest % 10);
        rest /= 10;
    } while (rest != 0);
    if (value < 0)
        console("-");
    while (count != 0)
        console({&digits[--count], 1});
}

// One line per started CPU, in TSC cycles: the first measurement, the
// corrections and the last measurement.
void report_tsc(ok::smp::CpuId cpu, u32 features, const tsc::Outcome &outcome) noexcept
{
    console("OK_TSC_SYNC scope=hardware cpu=");
    console_signed(cpu);
    console(" result=");
    console(tsc::name(outcome.verdict));
    console((features & feature_invariant) != 0 ? " invariant=1" : " invariant=0");
    console((features & feature_tsc_adjust) != 0 ? " tsc_adjust=1" : " tsc_adjust=0");
#if defined(OK_X86_TSC_SKEW_TEST)
    console(" skew_test=");
    console(cpu != 1 ? "none" : skew_injected.load(std::memory_order_relaxed) == skew_tsc ? "tsc" : "software");
#endif
    const auto field = [](std::string_view name, i64 value) {
        console(name);
        console_signed(value);
    };
    if (outcome.initial.valid())
    {
        field(" offset=", outcome.initial.offset);
        field(" error=", static_cast<i64>(outcome.initial.error));
    }
    field(" adjusted=", outcome.adjusted);
    field(" software=", outcome.offset);
    if (outcome.final.valid())
    {
        field(" residual=", outcome.final.offset);
        field(" residual_error=", static_cast<i64>(outcome.final.error));
    }
    console("\n");
}

// The boot CPU's half, after the STARTUP IPIs: the CPU joins the channel
// within the ready timeout or is reported unsynced.
void synchronize_tsc(ok::smp::CpuId cpu) noexcept
{
    auto &channel = tsc_channel;
    const u64 timeout = cycles_for(sync_answer_timeout_ns);
    const u64 ready_timeout = cycles_for(sync_ready_timeout_ns);
    const u64 start = read_tsc();
    while (channel.ready.load(std::memory_order_acquire) != cpu && read_tsc() - start < ready_timeout)
        ArchOperations::spin_pause();
    tsc::Outcome outcome{};
    u32 features = boot_tsc_features;
    if (channel.ready.load(std::memory_order_acquire) == cpu)
    {
        features &= channel.features.load(std::memory_order_relaxed);
        BootLink link{cpu, (features & feature_tsc_adjust) != 0, timeout};
        outcome = tsc::synchronize(link, {.rounds = 64, .slack = cycles_for(sync_slack_ns)});
        if (!link.finish())
            outcome.verdict = tsc::Verdict::unsynced;
    }
    // A CPU still following (it stopped answering) leaves on seeing this.
    channel.target.store(0, std::memory_order_release);
    tsc_verdict = tsc_synchronized ? tsc::worse(tsc_verdict, outcome.verdict) : outcome.verdict;
    tsc_synchronized = true;
    report_tsc(cpu, features, outcome);
}

#if defined(OK_X86_TSC_SKEW_TEST)
// Debug knob: CPU 1 enters the handshake with its TSC rewound to 0x10 (where
// IA32_TSC_ADJUST exists, the write moves it too). A CPU that ignores the
// write (QEMU TCG does) gets the same rewind in its clock reads' offset.
void inject_skew(ok::smp::CpuId cpu) noexcept
{
    if (cpu != 1)
        return;
    constexpr u32 msr_tsc = 0x10;
    constexpr u64 rewound = 0x10;
    const u64 before = execution::x86::read_tsc_ordered();
    write_msr(msr_tsc, rewound);
    const u64 after = execution::x86::read_tsc_ordered();
    if (after < before / 2)
    {
        skew_injected.store(skew_tsc, std::memory_order_relaxed);
        return;
    }
    execution::x86::this_cpu().tsc_offset = after - rewound;
    skew_injected.store(skew_software, std::memory_order_relaxed);
}
#endif

// The starting CPU's half, on its own CpuLocal: answer until the boot CPU
// finishes or gives up on this CPU.
void follow_tsc_sync(ok::smp::CpuId cpu) noexcept
{
    auto &channel = tsc_channel;
    if (channel.target.load(std::memory_order_acquire) != cpu)
        return;
#if defined(OK_X86_TSC_SKEW_TEST)
    inject_skew(cpu);
#endif
    const u32 features = tsc_features();
    channel.features.store(features, std::memory_order_relaxed);
    channel.ready.store(cpu, std::memory_order_release);
    u32 seen = 0;
    for (;;)
    {
        u32 command;
        while ((command = channel.command.load(std::memory_order_acquire)) >> 3 == seen)
        {
            if (channel.target.load(std::memory_order_acquire) != cpu)
                return;
            ArchOperations::spin_pause();
        }
        seen = command >> 3;
        bool refused = false;
        const auto argument = channel.argument.load(std::memory_order_relaxed);
        switch (command & 7)
        {
        case sync_sample:
            channel.reading.store(execution::x86::clock_counter(), std::memory_order_relaxed);
            break;
        case sync_adjust:
            // TSC_ADJUST moves the TSC by the same amount it changes by.
            if ((features & feature_tsc_adjust) != 0)
                write_msr(msr_tsc_adjust, read_msr(msr_tsc_adjust) + argument);
            else
                refused = true;
            break;
        case sync_add_offset:
            execution::x86::this_cpu().tsc_offset += argument;
            break;
        default:
            break;
        }
        channel.answer.store(answer_tag(seen, cpu) | (refused ? 1u : 0u), std::memory_order_release);
        if ((command & 7) == sync_finish)
            return;
    }
}

} // namespace

Status discover(boot::CpuTopologyMap &map)
{
    const auto up = [&](std::string_view reason) {
        map = {};
        map.count = 1;
        map.cpus[0] = {.hardware_id = current_hardware_id(), .enabled = true, .boot = true};
        map.boot_known = true;
        map.up_reason = reason;
        return Status::success();
    };
    const boot::PhysicalReader reader{&identity_window, nullptr};
    u64 rsdp = 0;
    if (!boot::find_acpi_rsdp(reader, rsdp))
        return up("acpi-absent");
    boot::CpuTopologyMap parsed{};
    if (auto status = boot::parse_acpi_madt(reader, rsdp, parsed); !status)
        return status;
    if (parsed.method != boot::CpuStartMethod::x86_sipi)
        return up(parsed.up_reason.empty() ? std::string_view{"single-cpu"} : parsed.up_reason);
    // Only the xAPIC page boot.S maps can start and interrupt CPUs.
    const u64 apic_base = read_msr(0x1b);
    if (parsed.local_apic_address != lapic::base || (apic_base & ~u64{0xfff}) != lapic::base ||
        (apic_base & (u64{1} << 11)) == 0 || (apic_base & (u64{1} << 10)) != 0)
        return up("xapic-unavailable");
    for (usize i = 0; i < parsed.count; ++i)
        if (parsed.cpus[i].enabled && parsed.cpus[i].hardware_id > 0xfe)
            return up("x2apic-id");
    // A sanity bound only: the MADT stays the description.
    if (const auto firmware = fw_cfg_cpu_count(); firmware != 0 && parsed.enabled_count() > firmware)
        return up("madt-exceeds-fw-cfg");
    static_cast<void>(boot::mark_boot_cpu(parsed, current_hardware_id()));
    map = parsed;
    return Status::success();
}

std::string_view start_method_name(const boot::CpuTopologyMap &map) noexcept
{
    return map.method == boot::CpuStartMethod::x86_sipi ? std::string_view{"sipi"} : std::string_view{"unsupported"};
}

Status prepare_boot_cpu(const boot::CpuTopologyMap &)
{
    execution::x86::ok_x86_cpu_locals[0].apic_id = current_hardware_id();
    boot_tsc_features = tsc_features();
    // The 8259 keeps delivering through LINT0 as ExtINT, which needs no local
    // APIC EOI, so the PIT tick and device lines stay on this CPU.
    lapic::reg(lapic::task_priority) = 0;
    lapic::reg(lapic::lvt_timer) = lapic::lvt_masked | lapic::timer_vector;
    lapic::reg(lapic::lvt_error) = lapic::lvt_masked;
    lapic::reg(lapic::lvt_lint0) = delivery_extint;
    lapic::reg(lapic::lvt_lint1) = delivery_nmi;
    lapic::reg(lapic::spurious) = apic_software_enable | lapic::spurious_vector;
    return calibrate_timer();
}

u64 current_hardware_id() noexcept
{
    u32 a = 1, b, c, d;
    asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return b >> 24; // Initial APIC id.
}

Status start(ok::smp::CpuId cpu, u64 hardware_id, uptr, uptr stack_top)
{
    uptr cr0, cr3, cr4;
    asm volatile("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2" : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    if (hardware_id > 0xfe || cpu == 0 || cpu >= ok::smp::max_cpus)
        return Status::invalid_argument("x86 secondary start needs an xAPIC id and a secondary slot");
    if ((cr3 >> 32) != 0)
        return Status::unsupported("the boot PML4 lies above the trampoline's 4 GiB reach");
    if (stack_top > identity_limit || (stack_top & 15) != 0)
        return Status::invalid_argument("x86 secondary stack lies outside the boot identity map");
    execution::x86::prepare_cpu(cpu, static_cast<u32>(hardware_id));

    const usize size = static_cast<usize>(ok_x86_smp_trampoline_end - ok_x86_smp_trampoline);
    auto *page = reinterpret_cast<volatile u8 *>(trampoline_base);
    for (usize i = 0; i < size; ++i)
        page[i] = ok_x86_smp_trampoline[i];
    auto &parameters = *reinterpret_cast<volatile TrampolineParameters *>(
        trampoline_base + static_cast<uptr>(ok_x86_smp_trampoline_parameters - ok_x86_smp_trampoline));
    parameters.arrived = 0;
    parameters.cr0 = cr0;
    parameters.cr3 = cr3;
    parameters.cr4 = cr4;
    parameters.entry = reinterpret_cast<uptr>(&ok_x86_smp_secondary_entry);
    parameters.stack = stack_top;
    parameters.cpu = cpu;
    tsc_channel.command.store(0, std::memory_order_relaxed);
    tsc_channel.ready.store(0, std::memory_order_relaxed);
    tsc_channel.answer.store(0, std::memory_order_relaxed);
    tsc_channel.target.store(cpu, std::memory_order_release);
    asm volatile("mfence" : : : "memory");

    // INIT, then STARTUP at the trampoline page; a second STARTUP only if the
    // first was not taken.
    send_icr(hardware_id, delivery_init_assert);
    send_icr(hardware_id, delivery_init_deassert);
    delay_ns(10'000'000);
    constexpr u32 vector = trampoline_base >> 12;
    send_icr(hardware_id, delivery_startup | vector);
    delay_ns(200'000);
    if (parameters.arrived == 0)
    {
        send_icr(hardware_id, delivery_startup | vector);
        delay_ns(200'000);
    }
    // A CPU whose TSC cannot be verified still starts: the shared clock's
    // floor keeps time from going back on it.
    synchronize_tsc(cpu);
    return Status::success();
}

std::string_view clock_sync_name() noexcept
{
    return tsc_synchronized ? tsc::name(tsc_verdict) : std::string_view{};
}

void send_ipi(ok::smp::CpuId, u64 hardware_id) noexcept
{
    // An interrupt on this CPU must not split the ICR's two writes.
    const auto saved = ArchOperations::save_and_disable_interrupts();
    send_icr(hardware_id, delivery_fixed | lapic::ipi_vector);
    ArchOperations::restore_interrupts(saved);
}

void wait_local() noexcept
{
    // The periodic local timer bounds the wait; its handler, or the IPI
    // handler, runs at the HLT.
    asm volatile("sti; hlt; cli" : : : "memory");
}

} // namespace ok::arch::smp

// The trampoline's 64-bit target, on the CPU's own stack and the boot PML4.
extern "C" [[noreturn]] void ok_x86_smp_secondary_entry(ok::smp::CpuId cpu)
{
    const ok::u32 mxcsr = 0x1f80;
    asm volatile("fninit; ldmxcsr %0" : : "m"(mxcsr) : "memory");
    ok::arch::execution::x86::load_cpu(cpu);
    ok::arch::smp::follow_tsc_sync(cpu);
    ok::arch::smp::prepare_secondary_apic();
    ok_smp_secondary_main(cpu);
}
