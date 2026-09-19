#include "ok/arch/execution.hpp"
#include <cstddef>

namespace ok::arch::execution
{
namespace
{
constexpr usize max_pages = 256;
constexpr uptr ram_end = 0x10000000;
constexpr uptr msr_user = 0x0200c000; // SPE, EE, PR; no unavailable classic FPU.
struct Space
{
    std::array<PageMapping, max_pages> mappings{};
    usize count{};
    u32 generation{};
    bool allocated{};
};
std::array<Space, max_address_spaces> spaces{};
bool initialized{};
bool executing{};
u32 next_tlb = 3;
extern "C" void ok_ppc_execution_enter(UserFrame *, Trap *);
extern "C" const u8 ok_ppc_vectors[];
extern "C" void ok_platform_console_write_char(char);
static_assert(offsetof(UserFrame, pc) == 128 && offsetof(UserFrame, sp) == 132);
static_assert(offsetof(UserFrame, flags) == 136 && offsetof(UserFrame, fp_state) == 160);
static_assert(offsetof(Trap, vector) == 4 && offsetof(Trap, error_code) == 8 && offsetof(Trap, fault_address) == 12);

Space *storage(const AddressSpace &space)
{
    if (space.slot >= spaces.size())
        return nullptr;
    auto &value = spaces[space.slot];
    return value.allocated && value.generation == space.generation && reinterpret_cast<uptr>(&value) == space.root
               ? &value
               : nullptr;
}
const PageMapping *mapping(const Space &space, uptr address)
{
    const auto page = address & ~(page_size - 1);
    for (usize i = 0; i < space.count; ++i)
        if (space.mappings[i].virtual_address == page)
            return &space.mappings[i];
    return nullptr;
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

bool supported() noexcept
{
    return true;
}
Status initialize()
{
    if (initialized)
        return Status::success();
    if ((read_spr<287>() >> 16) != 0x8021)
        return Status::unsupported("PPC native backend requires the ppce500 e500v2 CPU");
    uptr msr;
    asm volatile("mfmsr %0" : "=r"(msr));
    if ((msr & 0xc030) != 0)
        return Status::unsupported("PPC native initialization requires supervisor TS0 with interrupts masked");
    // QEMU's initial RAM mapping grants user access. Replace it before any
    // PR=1 transition, retaining the 256 MiB supervisor identity map.
    write_tlb(0, 0xc0000800, 0, 0x15);
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
    return Status::success();
}
Result<AddressSpace> create_space(std::span<const PageMapping> mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution backend is not initialized");
    if (mappings.empty() || mappings.size() > max_pages)
        return Status::invalid_argument("invalid PPC native page count");
    for (usize i = 0; i < mappings.size(); ++i)
    {
        const auto &page = mappings[i];
        if (page.virtual_address < user_begin || page.virtual_address >= user_end ||
            (page.virtual_address & (page_size - 1)) != 0 || page.physical_address < page_size ||
            page.physical_address >= ram_end || (page.physical_address & (page_size - 1)) != 0 ||
            (page.flags & page_read) == 0 || (page.flags & ~(page_read | page_write | page_execute)) != 0 ||
            (page.flags & (page_write | page_execute)) == (page_write | page_execute))
            return Status::invalid_argument("invalid PPC native mapping or writable executable page");
        for (usize j = 0; j < i; ++j)
            if (mappings[j].virtual_address == page.virtual_address)
                return Status::already_exists("duplicate PPC native page");
    }
    usize slot = 0;
    while (slot < spaces.size() && spaces[slot].allocated)
        ++slot;
    if (slot == spaces.size())
        return Status::no_memory("native address space slots exhausted");
    auto &value = spaces[slot];
    if (++value.generation == 0)
        ++value.generation;
    value.count = mappings.size();
    for (usize i = 0; i < mappings.size(); ++i)
    {
        value.mappings[i] = mappings[i];
        if ((mappings[i].flags & page_execute) != 0)
        {
            // The caller populated code using data stores through a physical
            // alias. Publish those stores before first instruction fetch.
            for (uptr address = mappings[i].physical_address; address < mappings[i].physical_address + page_size;
                 address += 32)
                asm volatile("dcbst 0,%0" : : "r"(address) : "memory");
            asm volatile("sync" : : : "memory");
            for (uptr address = mappings[i].physical_address; address < mappings[i].physical_address + page_size;
                 address += 32)
                asm volatile("icbi 0,%0" : : "r"(address) : "memory");
            asm volatile("sync; isync" : : : "memory");
        }
    }
    value.allocated = true;
    return AddressSpace{reinterpret_cast<uptr>(&value), static_cast<u32>(slot), value.generation};
}
Status destroy_space(AddressSpace &space)
{
    auto *value = storage(space);
    if (value == nullptr)
        return Status::not_found("native address-space handle is stale");
    if (executing)
        return Status::busy("cannot destroy an executing address space");
    value->allocated = false;
    space = {};
    return Status::success();
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
        if (entry == nullptr || (write && (entry->flags & page_write) == 0))
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
Result<Trap> enter(AddressSpace &space, UserFrame &frame)
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
        ok_ppc_execution_enter(&frame, &trap);
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
                : (trap.vector == 2 || trap.vector == 3 || trap.vector == 13 || trap.vector == 14)
                    ? TrapKind::page_fault
                    : TrapKind::exception;
    if (trap.vector == 3 || trap.vector == 14)
        trap.fault_address = frame.pc;
    return trap;
}
} // namespace ok::arch::execution
