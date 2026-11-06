#include "ok/arch/execution.hpp"
#include "ok/arch/smp.hpp"
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
template <typename Done> bool wait_for(Done done)
{
    const u64 start = arch::riscv::Clint::time();
    while (!done())
        if (arch::riscv::Clint::time() - start > 20'000'000)
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
// mmap churn: tags roll over and are reused, yet each space counts only in its
// own frame (three frames rotate, so a reused tag's stale entry would name
// another one), and the next entry after a remap counts in the new frame.
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
    check(hw::remap(kept, kept_pages[1]).ok(), "remap long-lived page");
    check(count_through_user(kept) == 501 && remap_target[0] == 501 && kept_data[0] == kept_count,
          "next entry after a remap uses the new frame");
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

// Hart 1's share: it runs the counting loop in its own space until hart 0
// asks it to stop, and reports how each entry ended.
alignas(16) std::array<std::byte, 16384> hart_one_stack{};
hw::AddressSpace hart_one_space{};
std::atomic<u32> hart_one_phase{}; // 1 run, 2 stop, 3 stopped
std::atomic<u32> hart_one_timers{}, hart_one_ipis{}, hart_one_other{};
std::atomic<uptr> hart_one_count{};
std::atomic<bool> hart_one_identity{};
} // namespace
extern "C" [[noreturn]] void ok_smp_secondary_main(smp::CpuId cpu)
{
    auto &record = smp::cpu_record(cpu);
    hart_one_identity.store(smp::current_cpu() == cpu, std::memory_order_relaxed);
    record.reported_id.store(static_cast<uptr>(arch::smp::current_hardware_id()), std::memory_order_release);
    record.state.store(static_cast<u8>(smp::CpuState::online), std::memory_order_release);
    while (hart_one_phase.load(std::memory_order_acquire) == 0)
        arch::ArchOperations::spin_pause();
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
        else
            hart_one_other.fetch_add(1, std::memory_order_relaxed);
    }
    hart_one_count.store(frame.registers[8], std::memory_order_relaxed);
    hart_one_phase.store(3, std::memory_order_release);
    for (;;)
        arch::smp::wait_local();
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
    check(wait_for([&] { return hart_one.local_ticks.load(std::memory_order_relaxed) >= 2; }), "hart 1 local timer");
    trap = hw::enter(space, memory_a);
    check(trap && trap.value().kind == hw::TrapKind::syscall && memory_a.registers[10] == value + 3,
          "user entry beside hart 1");
    // The shootdown waits only for an open user window, which hart 1 leaves
    // for microseconds per 10 ms quantum; retry until one was waited for.
    for (usize attempt = 0; attempt < 100 && smp::cpu_record(0).shootdowns.load() == 0; ++attempt)
        check(hw::remap(second.value(), other_pages[1]).ok(), "remap beside hart 1");
    check(smp::cpu_record(0).shootdowns.load() != 0 &&
              wait_for([&] { return hart_one_ipis.load(std::memory_order_relaxed) != 0; }),
          "shootdown IPI takes hart 1 out of U-mode");
    hart_one_phase.store(2, std::memory_order_release);
    check(wait_for([&] { return hart_one_phase.load(std::memory_order_acquire) == 3; }) &&
              hart_one_timers.load() >= 2 && hart_one_other.load() == 0 && hart_one_count.load() != 0 &&
              smp::cpu_record(0).local_ticks.load() == 0,
          "hart 1 user traps");
    const auto received = hart_one.ipis_received.load(std::memory_order_acquire);
    smp::send_ipi(1, smp::ipi_ping);
    check(wait_for([&] { return hart_one.ipis_received.load(std::memory_order_acquire) != received; }),
          "IPI wakes hart 1's idle wait");
    emit("RV64_NATIVE harts=2 start=pass local_timer=pass shootdown=pass idle_ipi=pass\n");
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
