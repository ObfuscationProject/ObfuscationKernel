
#include "ok/user/native_signal_config.hpp"
#include "ok/interrupt/platform.hpp"
#include "ok/arch/execution_spaces.hpp"
#include <cstddef>
#if defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__powerpc__) || defined(__PPC__)) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define OK_PPC32_PRIVATE_BACKEND 1
#include "ok/arch/ppc32_private_signal.hpp"
#endif

namespace ok::arch::execution
{
namespace
{
constexpr uptr ram_end = 0x10000000;
constexpr uptr msr_user = 0x0200c000; // SPE, EE, PR; no unavailable classic FPU.
// Refill reads the runtime-owned records directly; a space needs no tables.
struct Space
{
    MappingView mappings{};
};
constinit SpaceTable<Space> spaces;
bool initialized{};
bool executing{};
u32 next_tlb = 3;
extern "C" void ok_ppc_execution_enter(UserFrame *, Trap *, uptr);
extern "C" const u8 ok_ppc_vectors[];
extern "C" void ok_platform_console_write_char(char);
static_assert(offsetof(UserFrame, pc) == 128 && offsetof(UserFrame, sp) == 132);
static_assert(offsetof(UserFrame, flags) == 136 && offsetof(UserFrame, fp_state) == 160);
static_assert(offsetof(Trap, kernel_stack_pointer) == 16);
static_assert(offsetof(Trap, vector) == 4 && offsetof(Trap, error_code) == 8 && offsetof(Trap, fault_address) == 12);

Space *storage(const AddressSpace &space)
{
    return spaces.find(space);
}
const PageMapping *mapping(const Space &space, uptr address)
{
    const auto *page = space.mappings.find(address);
    return page && page->flags ? page : nullptr;
}
bool admissible(const PageMapping &page)
{
    return page.virtual_address >= user_begin && page.virtual_address < user_end &&
           (page.virtual_address & (page_size - 1)) == 0 && page.physical_address >= page_size &&
           page.physical_address < ram_end && (page.physical_address & (page_size - 1)) == 0 &&
           (page.flags == 0 || (page.flags & page_read) != 0) &&
           (page.flags & ~(page_read | page_write | page_execute)) == 0 &&
           (page.flags & (page_write | page_execute)) != (page_write | page_execute);
}
void publish_code(uptr physical)
{
    // The caller populated code using data stores through a physical alias.
    // Publish those stores before first instruction fetch.
    for (uptr address = physical; address < physical + page_size; address += 32)
        asm volatile("dcbst 0,%0" : : "r"(address) : "memory");
    asm volatile("sync" : : : "memory");
    for (uptr address = physical; address < physical + page_size; address += 32)
        asm volatile("icbi 0,%0" : : "r"(address) : "memory");
    asm volatile("sync; isync" : : : "memory");
}
void write_tlb(u32 index, u32 mas1, u32 mas2, u32 mas3, u32 mas7 = 0)
{
    const u32 mas0 = 0x10000000 | (index << 16);
    asm volatile("mtspr 624,%0; mtspr 625,%1; mtspr 626,%2; mtspr 627,%3; mtspr 944,%4; isync; tlbwe; isync"
                 :
                 : "r"(mas0), "r"(mas1), "r"(mas2), "r"(mas3), "r"(mas7)
                 : "memory");
}
void invalidate_user_tlb()
{
    for (u32 i = 3; i < 16; ++i)
        write_tlb(i, 0, 0, 0);
    next_tlb = 3;
}
void load_page(const PageMapping &page)
{
    // TLB1 is a fully associative hardware cache. Software refills misses from
    // the current address space; only slots 3..15 can contain user mappings.
    u32 permissions = 0x03; // SR, UR.
    if ((page.flags & page_write) != 0)
        permissions |= 0x0c; // SW, UW.
    if ((page.flags & page_execute) != 0)
        permissions |= 0x30; // SX, UX.
    write_tlb(next_tlb, 0x80000100, page.virtual_address, page.physical_address | permissions);
    if (++next_tlb == 16)
        next_tlb = 3;
}
template <u32 Spr> u32 read_spr()
{
    u32 value;
    asm volatile("mfspr %0,%1" : "=r"(value) : "i"(Spr));
    return value;
}
template <u32 Spr> void write_spr(u32 value)
{
    asm volatile("mtspr %0,%1" : : "i"(Spr), "r"(value) : "memory");
}
template <u32 Vector> void set_vector(uptr base)
{
    write_spr<400 + Vector>(base & 0xffff);
}
} // namespace
#ifdef OK_PPC32_PRIVATE_BACKEND
namespace ppc32_private::detail
{
struct BackendAccess final
{
    static Tracker tracker;
    static Facts measured;
    static Trap raw;
    static void initialize_completed() noexcept
    {
        measured={};
        if(!initialized) {tracker.invalidate();return;}
        measured.pvr=read_spr<287>();
        asm volatile("mfmsr %0":"=r"(measured.msr));
        if(measured.pvr==0x80210022)
        { measured.spefscr=read_spr<512>();measured.spefscr_read=true; }
        tracker.qualify_initialized(measured);
    }
    static void invalidate() noexcept {tracker.invalidate();raw={};}
    static void capture(const UserFrame &frame,AddressSpace space,const KernelEntryStackView &view) noexcept
    {tracker.publish_owned(frame,space,view,raw);}
};
Tracker BackendAccess::tracker{};
Facts BackendAccess::measured{};
Trap BackendAccess::raw{};
}
namespace ppc32_private
{
Capability capability() noexcept {return detail::BackendAccess::tracker.capability();}
Observation observation(const UserFrame &frame) noexcept {return detail::BackendAccess::tracker.query(frame);}
Facts facts() noexcept {return detail::BackendAccess::measured;}
bool address_space_live(AddressSpace space) noexcept {return storage(space)!=nullptr;}
}
#endif

bool supported() noexcept
{
    return true;
}
OwnershipReport ownership_report() noexcept
{
    OwnershipReport report{};
    report.executing = executing;
    spaces.each([&](const Space &space, uptr root) {
        ++report.address_spaces;
        if (space.mappings.empty() || !space.mappings.valid() || root != reinterpret_cast<uptr>(space.mappings.records))
            report.failure = Status::io_error("native backend has an inconsistent mapping owner");
    });
    report.table_pages = spaces.pages();
    if (!initialized && (report.address_spaces || report.executing))
        report.failure = Status::io_error("uninitialized native backend retains owners");
    return report;
}

Status initialize()
{
    if (initialized)
    {
#ifdef OK_PPC32_PRIVATE_BACKEND
        ppc32_private::detail::BackendAccess::initialize_completed();
#endif
        return Status::success();
    }
    if ((read_spr<287>() >> 16) != 0x8021)
        return Status::unsupported("PPC native backend requires the ppce500 e500v2 CPU");
    uptr msr;
    asm volatile("mfmsr %0" : "=r"(msr));
    if ((msr & 0xc030) != 0)
        return Status::unsupported("PPC native initialization requires supervisor TS0 with interrupts masked");
    // QEMU's initial RAM mapping grants user access. Replace it before any
    // PR=1 transition, retaining the 256 MiB supervisor identity map.
    write_tlb(0, 0xc0000900, 0, 0x15);
    invalidate_user_tlb();
    const auto vectors = reinterpret_cast<uptr>(ok_ppc_vectors);
    write_spr<63>(vectors & 0xffff0000);
#define OK_PPC_VECTOR(n) set_vector<n>(vectors + (n) * 32)
    OK_PPC_VECTOR(0);
    OK_PPC_VECTOR(1);
    OK_PPC_VECTOR(2);
    OK_PPC_VECTOR(3);
    OK_PPC_VECTOR(4);
    OK_PPC_VECTOR(5);
    OK_PPC_VECTOR(6);
    OK_PPC_VECTOR(7);
    OK_PPC_VECTOR(8);
    OK_PPC_VECTOR(9);
    OK_PPC_VECTOR(10);
    OK_PPC_VECTOR(11);
    OK_PPC_VECTOR(12);
    OK_PPC_VECTOR(13);
    OK_PPC_VECTOR(14);
    OK_PPC_VECTOR(15);
#undef OK_PPC_VECTOR
    write_spr<528>((vectors + 16 * 32) & 0xffff); // SPE unavailable.
    write_spr<529>((vectors + 17 * 32) & 0xffff); // SPE data exception.
    write_spr<530>((vectors + 18 * 32) & 0xffff); // SPE round exception.
    initialized = true;
#ifdef OK_PPC32_PRIVATE_BACKEND
    ppc32_private::detail::BackendAccess::initialize_completed();
#endif
    return Status::success();
}
Result<AddressSpace> create_space(MappingView mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution backend is not initialized");
    if (!mappings.valid() || mappings.empty())
        return Status::invalid_argument("invalid PPC native mapping directory");
    for (const auto &page : mappings)
        if (!admissible(page))
            return Status::invalid_argument("invalid PPC native mapping or writable executable page");
    auto entry = spaces.acquire();
    if (!entry)
        return entry.status();
    for (const auto &page : mappings)
        if ((page.flags & page_execute) != 0)
            publish_code(page.physical_address);
    // The published records identify the space; no other live space names them.
    entry.value()->space.mappings = mappings;
    entry.value()->root = reinterpret_cast<uptr>(mappings.records);
    return spaces.handle(*entry.value());
}
Status remap(const AddressSpace &space, const PageMapping &mapping)
{
    auto *value = storage(space);
    if (value == nullptr)
        return Status::not_found("native address-space handle is stale");
    if (executing)
        return Status::busy("cannot remap an executing address space");
    if (!admissible(mapping) || mapping.flags == 0)
        return Status::invalid_argument("invalid PPC native remap");
    if (::ok::arch::execution::mapping(*value, mapping.virtual_address) == nullptr)
        return Status::not_found("native user page has no translation");
    // Misses refill from the runtime-owned records, and native entry clears
    // the user TLB slots, so only a new executable frame needs publishing.
    if ((mapping.flags & page_execute) != 0)
        publish_code(mapping.physical_address);
    return Status::success();
}
Status destroy_space(AddressSpace &space)
{
    auto *value = storage(space);
    if (value == nullptr)
        return Status::not_found("native address-space handle is stale");
    if (executing)
        return Status::busy("cannot destroy an executing address space");
    const auto retired = spaces.release(*value);
    space = {};
    return retired;
}
UserFrame initial_frame(uptr entry, uptr stack)
{
    UserFrame frame{};
    frame.pc = entry;
    frame.sp = stack;
    frame.flags = msr_user;
    return frame;
}
Syscall syscall(const UserFrame &frame)
{
    return {frame.registers[0],
            {frame.registers[3], frame.registers[4], frame.registers[5], frame.registers[6], frame.registers[7],
             frame.registers[8]}};
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[3] = static_cast<uptr>(value);
}
bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    auto *value = storage(space);
    if (value == nullptr)
        return false;
    if (size == 0)
        return true;
    if (address < user_begin || address >= user_end || size > user_end - address)
        return false;
    for (auto page = address & ~(page_size - 1); page <= address + size - 1; page += page_size)
    {
        const auto *entry = mapping(*value, page);
        if (entry == nullptr ||
            (write && (entry->flags & page_write) == 0 && !(resolve_write(space, page) && (entry->flags & page_write))))
            return false;
    }
    return true;
}
Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size)
{
    if ((destination == nullptr && size != 0) || !user_range(space, source, size))
        return Status::fault("user source range is not readable");
    auto *out = static_cast<u8 *>(destination);
    while (size != 0)
    {
        const auto offset = source & (page_size - 1);
        const auto count = size < page_size - offset ? size : page_size - offset;
        const auto *in = reinterpret_cast<const u8 *>(mapping(*storage(space), source)->physical_address + offset);
        for (usize i = 0; i < count; ++i)
            out[i] = in[i];
        out += count;
        source += count;
        size -= count;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size)
{
    if ((source == nullptr && size != 0) || !user_range(space, destination, size, true))
        return Status::fault("user destination range is not writable");
    auto *in = static_cast<const u8 *>(source);
    while (size != 0)
    {
        const auto offset = destination & (page_size - 1);
        const auto count = size < page_size - offset ? size : page_size - offset;
        auto *out = reinterpret_cast<u8 *>(mapping(*storage(space), destination)->physical_address + offset);
        for (usize i = 0; i < count; ++i)
            out[i] = in[i];
        in += count;
        destination += count;
        size -= count;
    }
    return Status::success();
}
static Result<Trap> enter_impl(AddressSpace &space, UserFrame &frame, uptr stack_top)
{
    const auto *value = storage(space);
    if (!initialized || value == nullptr)
        return Status::not_initialized("native address space is unavailable");
    if (executing)
        return Status::busy("PPC native backend currently supports one executing CPU");
    const auto *code = mapping(*value, frame.pc);
    if (code == nullptr || (code->flags & page_execute) == 0 || (frame.pc & 3) != 0 || frame.sp <= user_begin ||
        frame.sp > user_end || !user_range(space, frame.sp - 1, 1, true))
        return Status::fault("native entry or stack has invalid permissions");
    uptr kernel_msr;
    asm volatile("mfmsr %0" : "=r"(kernel_msr));
    if ((kernel_msr & 0x8000) != 0)
        return Status::unsupported("PPC synchronous native entry requires kernel interrupts masked");
    frame.flags = msr_user;
    executing = true;
    const u32 sprgs[] = {read_spr<272>(), read_spr<273>(), read_spr<274>(), read_spr<275>(),
                         read_spr<276>(), read_spr<277>(), read_spr<278>()};
    const auto old_tcr = read_spr<340>();
    const auto old_dec = read_spr<22>();
    invalidate_user_tlb();
    write_spr<336>(0x08000000); // Clear TSR.DIS.
    write_spr<22>(4000000);     // ppce500 timebase 400 MHz: 10 ms quantum.
    write_spr<340>(0x04000000); // TCR.DIE; no automatic reload.
    Trap trap{};
    for (;;)
    {
        ok_ppc_execution_enter(&frame, &trap, stack_top);
        if (trap.vector != 13 && trap.vector != 14)
            break;
        const auto fault = trap.vector == 14 ? frame.pc : trap.fault_address;
        const auto *page = mapping(*value, fault);
        const bool write = (trap.error_code & 0x00800000) != 0; // ESR.ST.
        if (page == nullptr || (trap.vector == 14 && (page->flags & page_execute) == 0) ||
            (write && (page->flags & page_write) == 0))
            break;
        load_page(*page);
    }
    write_spr<340>(old_tcr);
    write_spr<336>(0x08000000);
    write_spr<22>(old_dec);
    invalidate_user_tlb();
    write_spr<272>(sprgs[0]);
    write_spr<273>(sprgs[1]);
    write_spr<274>(sprgs[2]);
    write_spr<275>(sprgs[3]);
    write_spr<276>(sprgs[4]);
    write_spr<277>(sprgs[5]);
    write_spr<278>(sprgs[6]);
    executing = false;
    trap.kind = trap.vector == 8    ? TrapKind::syscall
                : trap.vector == 10 ? TrapKind::timer
                : trap.vector == 4  ? TrapKind::interrupt
                : (trap.vector == 2 || trap.vector == 3 || trap.vector == 13 || trap.vector == 14)
                    ? TrapKind::page_fault
                    : TrapKind::exception;
    // An external input (IVOR4) is claimed from the MPIC once the interrupt
    // service owns it; MSR[EE] in user mode lets only bound sources through.
    if (trap.kind == TrapKind::timer)
        ok_interrupt_tick();
    else if (trap.kind == TrapKind::interrupt)
        interrupt::claim_platform_irq();
#ifdef OK_PPC32_PRIVATE_BACKEND
    ppc32_private::detail::BackendAccess::raw=trap;
#endif
    if (trap.vector == 3 || trap.vector == 14)
        trap.fault_address = frame.pc;
    return trap;
}

Result<Trap> enter(AddressSpace &space, UserFrame &frame)
{
#ifdef OK_PPC32_PRIVATE_BACKEND
    ppc32_private::detail::BackendAccess::invalidate();
#endif
    return enter_impl(space, frame, 0);
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &stack)
{
#ifdef OK_PPC32_PRIVATE_BACKEND
    ppc32_private::detail::BackendAccess::invalidate();
#endif
    if (!stack.valid() || (stack.lower() & 15) || (stack.top() & 15) ||
        stack.top() <= stack.lower() || stack.top() - stack.lower() < 4096)
        return Status::invalid_argument("native entry requires a live owned kernel stack");
    auto result = enter_impl(space, frame, stack.top());
    if (result && result.value().kernel_stack_pointer &&
        (result.value().kernel_stack_pointer < stack.lower() || result.value().kernel_stack_pointer >= stack.top()))
        return Status::fault("privileged capture escaped the owned kernel stack");
#ifdef OK_PPC32_PRIVATE_BACKEND
    if(result) ppc32_private::detail::BackendAccess::capture(frame,space,stack);
#endif
    return result;
}

} // namespace ok::arch::execution
