#include "ok/arch/execution.hpp"
#include <cstddef>

namespace ok::arch::execution
{
namespace
{
static_assert(offsetof(UserFrame, pc) == sizeof(uptr) * 32);
static_assert(offsetof(UserFrame, fp_state) == (sizeof(uptr) == 8 ? 304 : 160));
struct Space
{
    std::array<PageMapping, 128> pages{};
    usize count{};
    u32 generation{};
    bool used{};
};
std::array<Space, max_address_spaces> spaces{};
bool initialized{}, executing{};
u32 tlb_entries{};
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
    if (space.slot >= spaces.size())
        return nullptr;
    auto &storage = spaces[space.slot];
    return storage.used && storage.generation == space.generation && reinterpret_cast<uptr>(&storage) == space.root
               ? &storage
               : nullptr;
}
const PageMapping *page_at(const Space &space, uptr address)
{
    for (usize i = 0; i < space.count; ++i)
        if (space.pages[i].virtual_address == (address & ~(page_size - 1)))
            return &space.pages[i];
    return nullptr;
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
    for (usize i = 0; i < space.count; ++i)
    {
        const auto &page = space.pages[i];
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
    void ok_mips_execution_enter(UserFrame *);
    void ok_mips_execution_vectors();
    UserFrame *ok_mips_execution_active_frame{};
    uptr ok_mips_execution_resume_sp{};
    uptr ok_mips_execution_cause{}, ok_mips_execution_bad_address{};
}
bool supported() noexcept
{
    return true;
}
Status initialize()
{
    if (initialized)
        return Status::success();
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
    write_cp0<15, 1>(reinterpret_cast<uptr>(&ok_mips_execution_vectors));
    write_cp0<13>(read_cp0<13>() & ~uptr(1u << 23)); // General interrupt vector.
    write_cp0<6>(0);
    write_cp0<7>(0); // RDHWR is emulated, including TLS, on both default CPUs.
    flush_tlb();
    initialized = true;
    return Status::success();
}
Result<AddressSpace> create_space(std::span<const PageMapping> mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution is not initialized");
    if (mappings.empty() || mappings.size() > 128)
        return Status::invalid_argument("invalid mapping count");
    for (usize i = 0; i < mappings.size(); ++i)
    {
        const auto &page = mappings[i];
        const auto segment = page.physical_address & segment_mask;
        if (page.virtual_address < user_begin || page.virtual_address >= user_end || (page.virtual_address & 4095) ||
            (page.physical_address & 4095) || (segment != kseg0 && segment != kseg1) || !(page.flags & page_read) ||
            (page.flags & ~7) || (page.flags & (page_write | page_execute)) == (page_write | page_execute))
            return Status::invalid_argument("invalid native mapping");
        for (usize j = 0; j < i; ++j)
            if (mappings[j].virtual_address == page.virtual_address)
                return Status::already_exists("duplicate mapping");
    }
    for (u32 slot = 0; slot < spaces.size(); ++slot)
    {
        auto &space = spaces[slot];
        if (space.used)
            continue;
        space.used = true;
        if (++space.generation == 0)
            ++space.generation;
        space.count = mappings.size();
        for (usize i = 0; i < mappings.size(); ++i)
            space.pages[i] = mappings[i];
        return AddressSpace{reinterpret_cast<uptr>(&space), slot, space.generation};
    }
    return Status::no_memory("native address space pool exhausted");
}
Status destroy_space(AddressSpace &space)
{
    auto *storage = lookup(space);
    if (!storage)
        return Status::invalid_argument("stale native address space");
    if (executing)
        return Status::busy("native execution is active");
    storage->used = false;
    storage->count = 0;
    space = {};
    return Status::success();
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame frame;
    frame.pc = pc;
    frame.sp = sp;
    frame.registers[29] = sp;
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
        if (!page || !(page->flags & page_read) || (write && !(page->flags & page_write)))
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
Result<Trap> enter(AddressSpace &space, UserFrame &frame)
{
    auto *storage = lookup(space);
    if (!storage || executing || frame.pc < user_begin || frame.pc >= user_end || (frame.pc & 3) ||
        frame.sp <= user_begin || frame.sp > user_end || (frame.sp & 7) || !user_range(space, frame.sp - 1, 1, true))
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
    Trap result;
    for (;;)
    {
        ok_mips_execution_enter(&frame);
        const auto cause = ok_mips_execution_cause;
        const u32 exception = (cause >> 2) & 31;
        const auto address = ok_mips_execution_bad_address;
        result = Trap{TrapKind::exception, exception, cause, 0};
        if (exception == 0 && (cause & 0x8000))
        {
            result.kind = TrapKind::timer;
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
    flush_tlb();
    ok_mips_execution_active_frame = nullptr;
    executing = false;
    write_cp0<12>(saved_status);
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
