
#include "ok/user/native_signal_config.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/arch/execution_spaces.hpp"
#include <cstddef>
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && defined(__mips64)
#define OK_PRIVATE_MIPS64_N64_ADMISSION 1
#include "ok/arch/mips64_n64_private_signal.hpp"
namespace ok::arch::execution::mips64_n64_private::detail { struct BackendAccess final { Tracker tracker{}; }; }
#endif
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && !defined(__mips64)
#define OK_PRIVATE_MIPS_O32_ADMISSION 1
#include "ok/arch/mips_o32_private_signal.hpp"
#endif

namespace ok::arch::execution
{
namespace
{
static_assert(offsetof(UserFrame, pc) == sizeof(uptr) * 32);
static_assert(offsetof(UserFrame, fp_state) == (sizeof(uptr) == 8 ? 304 : 160));
// Refill reads the runtime-owned records directly; a space needs no tables.
struct Space
{
    MappingView pages{};
};
constinit SpaceTable<Space> spaces;
bool initialized{}, executing{};
u32 tlb_entries{};
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
mips_o32_private::CaptureTracker private_signal_capture{};
mips_o32_private::Facts private_signal_facts{};
#endif
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
mips64_n64_private::detail::BackendAccess private_n64_backend{};
mips64_n64_private::Facts private_n64_facts{};
#endif
constexpr uptr segment_mask = ~uptr(0x1fffffff);
constexpr uptr kseg0 = static_cast<uptr>(static_cast<__INTPTR_TYPE__>(-2147483648LL));
constexpr uptr kseg1 = static_cast<uptr>(static_cast<__INTPTR_TYPE__>(-1610612736LL));
constexpr u32 timer_period = 1000000;
template <unsigned Register, unsigned Select = 0> uptr read_cp0()
{
    uptr value;
#if defined(__mips64)
    asm volatile("dmfc0 %0, $%1, %2" : "=r"(value) : "i"(Register), "i"(Select));
#else
    asm volatile("mfc0 %0, $%1, %2" : "=r"(value) : "i"(Register), "i"(Select));
#endif
    return value;
}
template <unsigned Register, unsigned Select = 0> void write_cp0(uptr value)
{
#if defined(__mips64)
    asm volatile("dmtc0 %0, $%1, %2; ehb" ::"r"(value), "i"(Register), "i"(Select) : "memory");
#else
    asm volatile("mtc0 %0, $%1, %2; ehb" ::"r"(value), "i"(Register), "i"(Select) : "memory");
#endif
}
Space *lookup(const AddressSpace &space)
{
    return spaces.find(space);
}
const PageMapping *page_at(const Space &space, uptr address)
{
    const auto *page = space.pages.find(address);
    return page && page->flags ? page : nullptr;
}
bool admissible(const PageMapping &page)
{
    const auto segment = page.physical_address & segment_mask;
    return page.virtual_address >= user_begin && page.virtual_address < user_end && !(page.virtual_address & 4095) &&
           !(page.physical_address & 4095) && (segment == kseg0 || segment == kseg1) &&
           (page.flags == 0 || (page.flags & page_read)) && !(page.flags & ~7) &&
           (page.flags & (page_write | page_execute)) != (page_write | page_execute);
}
uptr entry_lo(const PageMapping *page)
{
    // EntryLo has no execute-inhibit bit on the preserved Malta CPU profiles.
    return page ? ((page->physical_address & 0x1fffffff) >> 6) | 0x1a | ((page->flags & page_write) ? 4 : 0) : 0;
}
void flush_tlb()
{
    write_cp0<5>(0);
    write_cp0<2>(0);
    write_cp0<3>(0);
    for (u32 i = 0; i < tlb_entries; ++i)
    {
        write_cp0<0>(i);
        write_cp0<10>(kseg0 + i * 8192);
        asm volatile("tlbwi; ehb" ::: "memory");
    }
    write_cp0<10>(0);
}
void fill_tlb(const Space &space, uptr address)
{
    const uptr pair = address & ~uptr(8191);
    write_cp0<10>(pair);
    write_cp0<2>(entry_lo(page_at(space, pair)));
    write_cp0<3>(entry_lo(page_at(space, pair + page_size)));
    asm volatile("tlbwr; ehb" ::: "memory");
}
bool instruction(const Space &space, uptr address, u32 &value)
{
    const auto *page = page_at(space, address);
    if ((address & 3) || !page || !(page->flags & page_execute))
        return false;
    value = *reinterpret_cast<const u32 *>(page->physical_address + (address & 4095));
    return true;
}
bool continuation(const Space &space, const UserFrame &frame, uptr cause, uptr &next)
{
    next = frame.pc + 4;
    if (!(cause & 0x80000000u))
        return true;
    u32 branch;
    if (!instruction(space, frame.pc, branch))
        return false;
    #ifdef OK_PRIVATE_MIPS_O32_ADMISSION
    if (!mips_o32_private::delay_operands_admitted(branch)) return false;
#endif
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    if (!mips64_n64_private::delay_operands_admitted(branch)) return false;
#endif
    const u32 op = branch >> 26, rs = (branch >> 21) & 31, rt = (branch >> 16) & 31;
    const auto left = static_cast<__INTPTR_TYPE__>(frame.registers[rs]);
    bool take;
    switch (op)
    {
    case 0:
        if ((branch & 63) != 8 && (branch & 63) != 9)
            return false;
        next = frame.registers[rs];
        return true;
    case 2:
    case 3:
        next = ((frame.pc + 4) & ~uptr(0x0fffffff)) | ((branch & 0x03ffffffu) << 2);
        return true;
    case 4:
    case 20:
        take = frame.registers[rs] == frame.registers[rt];
        break;
    case 5:
    case 21:
        take = frame.registers[rs] != frame.registers[rt];
        break;
    case 6:
    case 22:
        take = left <= 0;
        break;
    case 7:
    case 23:
        take = left > 0;
        break;
    case 1:
        if (rt != 0 && rt != 1 && rt != 2 && rt != 3 && rt != 16 && rt != 17 && rt != 18 && rt != 19)
            return false;
        take = (rt & 1) ? left >= 0 : left < 0;
        break;
    case 17: {
        if (rs != 8)
            return false;
        const u32 fcsr = *reinterpret_cast<const u32 *>(frame.fp_state.data() + 256);
        const auto cc = (rt >> 2) & 7;
        const bool condition = (fcsr >> (cc ? 24 + cc : 23)) & 1;
        take = condition == static_cast<bool>(rt & 1);
        break;
    }
    default:
        return false;
    }
    const auto displacement = static_cast<i16>(branch & 0xffff);
    next = take ? frame.pc + 4 + static_cast<uptr>(static_cast<__INTPTR_TYPE__>(displacement) * 4) : frame.pc + 8;
    return true;
}
void synchronize_code(const Space &space)
{
    for (const auto &page : space.pages)
    {
        if (!(page.flags & page_execute))
            continue;
        for (uptr address = page.physical_address; address < page.physical_address + page_size; address += 16)
            asm volatile("cache 0x15, 0(%0); sync; cache 0x10, 0(%0)" ::"r"(address) : "memory");
    }
    asm volatile("sync; ehb" ::: "memory");
}
} // namespace
extern "C"
{
    // Status for user mode, read by the entry assembly.
#if defined(__mips64)
    constinit u32 ok_mips_user_status = 0x240080b3;
#else
    constinit u32 ok_mips_user_status = 0x20008013;
#endif
    void ok_mips_execution_enter(UserFrame *, uptr);
    void ok_mips_execution_vectors();
    UserFrame *ok_mips_execution_active_frame{};
    uptr ok_mips_execution_resume_sp{};
    uptr ok_mips_execution_cause{}, ok_mips_execution_bad_address{};
}
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
mips_o32_private::Capability mips_o32_private::capability() noexcept
{ return private_signal_capture.capability(); }
mips_o32_private::Observation mips_o32_private::observation(const UserFrame &frame) noexcept
{ return private_signal_capture.query(frame); }
mips_o32_private::Facts mips_o32_private::facts() noexcept
{ return private_signal_facts; }
#endif
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
mips64_n64_private::Capability mips64_n64_private::capability() noexcept
{ return private_n64_backend.tracker.capability(); }
mips64_n64_private::Observation mips64_n64_private::observation(const UserFrame &frame) noexcept
{ return private_n64_backend.tracker.query(frame); }
mips64_n64_private::Facts mips64_n64_private::facts() noexcept
{ return private_n64_facts; }
#endif
bool supported() noexcept
{
    return true;
}
OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    report.executing = executing || ok_mips_execution_active_frame != nullptr || ok_mips_execution_resume_sp != 0;
    spaces.each([&](const Space &space, uptr root) {
        ++report.address_spaces;
        if (space.pages.empty() || !space.pages.valid() || root != reinterpret_cast<uptr>(space.pages.records))
            report.failure = Status::io_error("native backend has an inconsistent mapping owner");
    });
    report.table_pages = spaces.pages();
    if (!initialized && (report.address_spaces || report.executing))
        report.failure = Status::io_error("uninitialized native backend retains owners");
    return report;
}

#if defined(__mips64)
constexpr u32 user_status = 0x240080b3; // Matches USER_STATUS in execution_asm.h.
#else
constexpr u32 user_status = 0x20008013;
#endif
Status initialize()
{
    if (initialized)
        return Status::success();
    #ifdef OK_PRIVATE_MIPS_O32_ADMISSION
    private_signal_facts = {};
    auto &facts = private_signal_facts;
    facts.prid = static_cast<u32>(read_cp0<15>());
    facts.config0 = static_cast<u32>(read_cp0<16>());
    if (facts.config0 & 0x80000000u) {
        facts.has_config1 = true; facts.config1 = static_cast<u32>(read_cp0<16, 1>());
        if (facts.config1 & 0x80000000u) {
            facts.has_config2 = true; facts.config2 = static_cast<u32>(read_cp0<16, 2>());
            if (facts.config2 & 0x80000000u) {
                facts.has_config3 = true; facts.config3 = static_cast<u32>(read_cp0<16, 3>());
            }
        }
    }
    // Config3.M must be zero for this exact profile; no absent select is read.
    if (!mips_o32_private::platform(facts))
        return Status::unsupported("private o32 signals require qualified 24Kf platform readback");
#endif
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    private_n64_facts={};
    auto &n64=private_n64_facts;
    n64.prid=static_cast<u32>(read_cp0<15>());n64.config0=static_cast<u32>(read_cp0<16>());
    if(n64.config0&0x80000000u) {
        n64.has_config1=true;n64.config1=static_cast<u32>(read_cp0<16,1>());
        if(n64.config1&0x80000000u) {
            n64.has_config2=true;n64.config2=static_cast<u32>(read_cp0<16,2>());
            if(n64.config2&0x80000000u) {
                n64.has_config3=true;n64.config3=static_cast<u32>(read_cp0<16,3>());
                if(n64.config3&0x80000000u) {
                    n64.has_config4=true;n64.config4=static_cast<u32>(read_cp0<16,4>());
                    if(n64.config4&0x80000000u) {n64.has_config5=true;n64.config5=static_cast<u32>(read_cp0<16,5>());}
                }
            }
        }
    }
    if(!mips64_n64_private::platform(n64))
        return Status::unsupported("private n64 signals require the qualified generic platform");
#endif
    const auto config = read_cp0<16, 1>();
    if (!(config & 1))
        return Status::unsupported("native MIPS execution requires an FPU");
    tlb_entries = ((config >> 25) & 63) + 1;
    // Keep the boot kernel in unmapped KSEG0. Only user addresses enter the TLB.
    uptr status = read_cp0<12>();
    status &= ~uptr(0x0040ffff); // BEV, interrupt masks, ERL/EXL, KSU and IE.
    status |= 0x20000000;        // CU1; the ABI permits kernel floating-point registers.
#if defined(__mips64)
    status |= 0x04000080; // FR=1 and KX for the n64 ABI.
#else
    status &= ~uptr(0x04000000); // O32 uses paired 32-bit floating-point registers.
#endif
    write_cp0<12>(status);
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
    facts.status = static_cast<u32>(read_cp0<12>());
    // Reject a failed CU1/FR0 write before any FPU instruction is attempted.
    if (facts.status != 0x20000000u)
        return Status::unsupported("private o32 signals require eager FR0 Status readback");
    asm volatile("cfc1 %0, $0" : "=r"(facts.fir));
    asm volatile("ctc1 $0, $31; ehb; cfc1 %0, $31" : "=r"(facts.fcsr) :: "memory");
    if (!private_signal_capture.qualify(facts))
        return Status::unsupported("private o32 signals require legacy scalar FPU readback");
#endif
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    n64.status=static_cast<u32>(read_cp0<12>());
    if(n64.status!=0x24000080u)
        return Status::unsupported("private n64 signals require live CU1 FR1 KX readback");
    asm volatile("cfc1 %0,$0":"=r"(n64.fir));
    asm volatile("cfc1 %0,$31":"=r"(n64.fcsr));
    if(!private_n64_backend.tracker.qualify(n64))
        return Status::unsupported("private n64 signals require clean legacy scalar FP state");
#endif
    write_cp0<15, 1>(reinterpret_cast<uptr>(&ok_mips_execution_vectors));
    write_cp0<13>(read_cp0<13>() & ~uptr(1u << 23)); // General interrupt vector.
    write_cp0<6>(0);
    write_cp0<7>(0); // RDHWR is emulated, including TLS, on both default CPUs.
    flush_tlb();
    initialized = true;
    return Status::success();
}
Result<AddressSpace> create_space(MappingView mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution is not initialized");
    if (!mappings.valid() || mappings.empty())
        return Status::invalid_argument("invalid mapping directory");
    for (const auto &page : mappings)
        if (!admissible(page))
            return Status::invalid_argument("invalid native mapping");
    auto entry = spaces.acquire();
    if (!entry)
        return entry.status();
    // The published records identify the space; no other live space names them.
    entry.value()->space.pages = mappings;
    entry.value()->root = reinterpret_cast<uptr>(mappings.records);
    return spaces.handle(*entry.value());
}
Status remap(const AddressSpace &space, const PageMapping &mapping)
{
    auto *storage = lookup(space);
    if (!storage)
        return Status::invalid_argument("stale native address space");
    if (executing)
        return Status::busy("native execution is active");
    if (!admissible(mapping) || !mapping.flags)
        return Status::invalid_argument("invalid native remap");
    // Refill reads the runtime-owned records, and native entry flushes the TLB
    // and synchronizes executable pages, so the record is the translation.
    return page_at(*storage, mapping.virtual_address) ? Status::success()
                                                      : Status::not_found("native user page has no translation");
}
Status destroy_space(AddressSpace &space)
{
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    private_n64_backend.tracker.invalidate();
#endif
    auto *storage = lookup(space);
    if (!storage)
        return Status::invalid_argument("stale native address space");
    if (executing)
        return Status::busy("native execution is active");
    const auto retired = spaces.release(*storage);
    space = {};
    return retired;
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame frame;
    frame.pc = pc;
    frame.sp = sp;
    frame.registers[29] = sp;
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    // Prospective fixed entry state; readiness still needs an explicit initial hook.
    if(private_n64_backend.tracker.capability().admitted()) frame.flags=mips64_n64_private::user_status;
#endif
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
    // Prospective fixed entry state, not a hardware trap capture.
    if (private_signal_capture.capability().admitted()) frame.flags = mips_o32_private::user_status;
#endif
    return frame;
}
Syscall syscall(const UserFrame &frame)
{
    Syscall call{.number = frame.registers[2]};
    for (usize i = 0; i < 6; ++i)
        call.arguments[i] = frame.registers[i + 4];
    return call;
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[2] = static_cast<uptr>(value);
}
bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    auto *storage = lookup(space);
    if (!storage)
        return false;
    if (!size)
        return true;
    if (address < user_begin || address > user_end || size > user_end - address)
        return false;
    while (size)
    {
        auto *page = page_at(*storage, address);
        if (!page || !(page->flags & page_read) ||
            (write && !(page->flags & page_write) &&
             !(resolve_write(space, address & ~(page_size - 1)) && (page->flags & page_write))))
            return false;
        usize chunk = page_size - (address & 4095);
        if (chunk > size)
            chunk = size;
        address += chunk;
        size -= chunk;
    }
    return true;
}
Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size)
{
    if ((size && !destination) || !user_range(space, source, size))
        return Status::fault("unreadable user range");
    auto *out = static_cast<u8 *>(destination);
    const auto *storage = lookup(space);
    while (size)
    {
        const auto offset = source & 4095;
        usize count = page_size - offset;
        if (count > size)
            count = size;
        const auto *in = reinterpret_cast<const u8 *>(page_at(*storage, source)->physical_address + offset);
        for (usize i = 0; i < count; ++i)
            out[i] = in[i];
        source += count;
        out += count;
        size -= count;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size)
{
    if ((size && !source) || !user_range(space, destination, size, true))
        return Status::fault("unwritable user range");
    auto *in = static_cast<const u8 *>(source);
    const auto *storage = lookup(space);
    while (size)
    {
        const auto offset = destination & 4095;
        usize count = page_size - offset;
        if (count > size)
            count = size;
        auto *out = reinterpret_cast<u8 *>(page_at(*storage, destination)->physical_address + offset);
        for (usize i = 0; i < count; ++i)
            out[i] = in[i];
        destination += count;
        in += count;
        size -= count;
    }
    return Status::success();
}
static Result<Trap> enter_impl(AddressSpace &space, UserFrame &frame, uptr stack_top)
{
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    private_n64_backend.tracker.invalidate();
#endif
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
    private_signal_capture.invalidate();
#endif
    auto *storage = lookup(space);
    // Like Linux, the stack need not be mapped or aligned; only an access to it
    // faults. A detached musl thread unmaps its own stack before its exit
    // syscall, and on o32 points sp at its code first.
    if (!storage || executing || frame.pc < user_begin || frame.pc >= user_end || (frame.pc & 3) ||
        frame.sp <= user_begin || frame.sp > user_end)
        return Status::invalid_argument("invalid native entry");
    if (const auto *code = page_at(*storage, frame.pc); code && !(code->flags & page_execute))
        return Trap{TrapKind::page_fault, 2, 0, frame.pc};
    const auto saved_status = read_cp0<12>();
    write_cp0<12>(saved_status & ~uptr(1));
    synchronize_code(*storage);
    flush_tlb();
    executing = true;
    ok_mips_execution_active_frame = &frame;
    write_cp0<11>(static_cast<u32>(read_cp0<9>()) + timer_period);
    // User mode takes the CP0 timer and, once the interrupt service owns the
    // 8259 pair, IP2.
    ok_mips_user_status = user_status | (interrupt::platform_irqs_started() ? 0x400u : 0u);
    Trap result;
    for (;;)
    {
        ok_mips_execution_enter(&frame, stack_top);
        const auto cause = ok_mips_execution_cause;
        const u32 exception = (cause >> 2) & 31;
        const auto address = ok_mips_execution_bad_address;
        result = Trap{TrapKind::exception, exception, cause, 0, ok_mips_execution_resume_sp};
        if (exception == 0 && (cause & 0x8000))
        {
            result.kind = TrapKind::timer;
            break;
        }
        if (exception == 0)
        {
            result.kind = TrapKind::interrupt;
            break;
        }
        if (exception == 1 || exception == 2 || exception == 3 || exception == 4 || exception == 5)
        {
            result.kind = TrapKind::page_fault;
            result.fault_address = address;
            const auto *page = page_at(*storage, address);
            if ((exception != 2 && exception != 3) || !page || (exception == 3 && !(page->flags & page_write)))
                break;
            fill_tlb(*storage, address);
            continue;
        }
        uptr next;
        if (exception == 8 && continuation(*storage, frame, cause, next))
        {
            frame.pc = next;
            result.kind = TrapKind::syscall;
            break;
        }
        // The Malta defaults do not provide CP0.UserLocal. Linux-compatible
        // RDHWR rt,$29 TLS reads are serviced through a real RI exception.
        u32 insn;
        const auto instruction_pc = frame.pc + ((cause & 0x80000000u) ? 4 : 0);
        if (exception == 10 && instruction(*storage, instruction_pc, insn) && (insn & 0xffe0ffffu) == 0x7c00e83bu &&
            continuation(*storage, frame, cause, next))
        {
            const auto destination = (insn >> 16) & 31;
            if (destination)
                frame.registers[destination] = frame.fs_base;
            frame.pc = next;
            continue;
        }
        break;
    }
    write_cp0<11>(0);
    if (result.kind == TrapKind::timer)
        ok_interrupt_tick();
    else if (result.kind == TrapKind::interrupt)
        interrupt::claim_platform_irq();
    flush_tlb();
    ok_mips_execution_active_frame = nullptr;
    ok_mips_execution_resume_sp = 0;
    executing = false;
    write_cp0<12>(saved_status);
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
    // A timer or a page fault leaves EPC at an instruction that has not
    // completed (the branch when Cause.BD is set), so the runtime may resume it
    // unchanged or, for a resolved fault, deliver a handler before resuming.
    using K = mips_o32_private::CaptureKind;
    const bool interrupted = result.kind == TrapKind::timer || result.kind == TrapKind::interrupt ||
                             result.kind == TrapKind::page_fault;
    private_signal_capture.publish(frame, result.kind == TrapKind::syscall ? K::syscall_normalized :
        interrupted ? ((result.error_code & 0x80000000u) ? K::interrupted_bd : K::interrupted_non_bd) : K::unverified);
#endif
    return result;
}

Result<Trap> enter(AddressSpace &space, UserFrame &frame)
{
    return enter_impl(space, frame, 0);
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &stack)
{
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    private_n64_backend.tracker.invalidate();
#endif
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
    private_signal_capture.invalidate();
#endif
    if (!stack.valid() || (stack.lower() & 15) || (stack.top() & 15) ||
        stack.top() <= stack.lower() || stack.top() - stack.lower() < 4096)
        return Status::invalid_argument("native entry requires a live owned kernel stack");
    auto result = enter_impl(space, frame, stack.top());
    if (result && result.value().kernel_stack_pointer &&
        (result.value().kernel_stack_pointer < stack.lower() || result.value().kernel_stack_pointer >= stack.top()))
        {
#ifdef OK_PRIVATE_MIPS_O32_ADMISSION
        private_signal_capture.invalidate();
#endif
        return Status::fault("privileged capture escaped the owned kernel stack");
    }
#ifdef OK_PRIVATE_MIPS64_N64_ADMISSION
    // Only a real capture inside the still-active minted view can issue proof.
    if(result) private_n64_backend.tracker.publish_owned(frame,space,stack,result.value());
#endif
    return result;
}

extern "C" [[noreturn]] void ok_mips_execution_kernel_fault()
{
    constexpr uptr uart = kseg1 + 0x180003f8;
    const char *message = "OK_NATIVE_KERNEL_FAULT mips\n";
    for (; *message; ++message)
    {
        while (!(*reinterpret_cast<volatile u8 *>(uart + 5) & 32))
        {
        }
        *reinterpret_cast<volatile u8 *>(uart) = static_cast<u8>(*message);
    }
    for (;;)
        asm volatile("nop");
}
} // namespace ok::arch::execution
