#include "ok/arch/execution.hpp"
#include <cstddef>

namespace ok::arch::execution
{
namespace
{
static_assert(offsetof(UserFrame, pc) == 256);
static_assert(offsetof(UserFrame, sp) == 264);
static_assert(offsetof(UserFrame, fp_state) == 304);
struct Space
{
    std::array<PageMapping, 128> pages{};
    usize count{};
    u32 generation{};
    bool used{};
};
std::array<Space, max_address_spaces> spaces{};
bool initialized{}, executing{};
Trap last_trap;
bool refill{};
uptr timer_ticks{1000000};
template <unsigned C> uptr read_csr()
{
    uptr value;
    asm volatile("csrrd %0, %1" : "=r"(value) : "i"(C));
    return value;
}
template <unsigned C> void write_csr(uptr value)
{
    asm volatile("csrwr %0, %1" : "+r"(value) : "i"(C) : "memory");
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
u64 entry(const PageMapping *page)
{
    if (!page)
        return 0;
    return page->physical_address | 1u | (3u << 2) | (1u << 4) | ((page->flags & page_write) ? 2 : 0) |
           ((page->flags & page_read) ? 0 : (u64{1} << 61)) | ((page->flags & page_execute) ? 0 : (u64{1} << 62));
}
void fill_tlb(const Space &space, uptr address)
{
    const uptr pair = address & ~uptr(8191);
    write_csr<0x11>(pair);
    write_csr<0x12>(entry(page_at(space, pair)));
    write_csr<0x13>(entry(page_at(space, pair + page_size)));
    write_csr<0x10>(12u << 24);
    asm volatile("tlbfill" : : : "memory");
}
} // namespace
extern "C"
{
    void ok_loong_execution_enter(UserFrame *);
    void ok_loong_execution_vector();
    void ok_loong_execution_refill();
    UserFrame *ok_loong_execution_active_frame{};
    uptr ok_loong_execution_resume_sp{};
}
bool supported() noexcept
{
    return true;
}
Status initialize()
{
    if (initialized)
        return Status::success();
    // The QEMU virt kernel boots with direct physical addressing. DMW0 keeps
    // the same addresses accessible only at PLV0 while user code uses the TLB.
    if ((read_csr<0>() & 3) != 0)
        return Status::unsupported("native setup requires PLV0");
    write_csr<2>(read_csr<2>() | 1u);
    write_csr<0x180>(0x11);
    write_csr<0xc>(reinterpret_cast<uptr>(&ok_loong_execution_vector));
    write_csr<0x88>(reinterpret_cast<uptr>(&ok_loong_execution_refill));
    write_csr<4>(1u << 11);
    write_csr<0x18>(0);
    write_csr<0x1e>(12);
    uptr frequency, ratio, index = 4;
    asm volatile("cpucfg %0, %1" : "=r"(frequency) : "r"(index));
    index = 5;
    asm volatile("cpucfg %0, %1" : "=r"(ratio) : "r"(index));
    if (frequency && (ratio >> 16))
        timer_ticks = frequency * (ratio & 0xffff) / (ratio >> 16) / 100;
    if (timer_ticks < 4)
        timer_ticks = 4;
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
        if (page.virtual_address < user_begin || page.virtual_address >= user_end || (page.virtual_address & 4095) ||
            (page.physical_address & 4095) || page.physical_address == 0 || page.physical_address >= (uptr{1} << 48) ||
            !(page.flags & page_read) || (page.flags & ~7) ||
            (page.flags & (page_write | page_execute)) == (page_write | page_execute))
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
    frame.registers[3] = sp;
    frame.flags = 7;
    return frame;
}
Syscall syscall(const UserFrame &frame)
{
    Syscall call{.number = frame.registers[11]};
    for (usize i = 0; i < 6; ++i)
        call.arguments[i] = frame.registers[i + 4];
    return call;
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[4] = static_cast<uptr>(value);
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
        const auto offset = source & (page_size - 1);
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
        const auto offset = destination & (page_size - 1);
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
    if (!storage || executing || frame.pc < user_begin || frame.pc >= user_end || frame.sp <= user_begin ||
        frame.sp > user_end || !user_range(space, frame.sp - 1, 1, true))
        return Status::invalid_argument("invalid native entry");
    // Reject non-executable continuation targets before ERTN. The hardware
    // PTE still carries NX for branches taken after entry.
    if (const auto *code = page_at(*storage, frame.pc); code && !(code->flags & page_execute))
        return Trap{TrapKind::page_fault, 6, 0, frame.pc};
    const auto saved_crmd = read_csr<0>();
    write_csr<0>(saved_crmd & ~uptr(4));
    asm volatile("invtlb 0, $zero, $zero; dbar 0; ibar 0" : : : "memory");
    executing = true;
    ok_loong_execution_active_frame = &frame;
    write_csr<0x44>(1);
    write_csr<0x41>((timer_ticks & ~uptr(3)) | 1);
    for (;;)
    {
        refill = false;
        ok_loong_execution_enter(&frame);
        if (!refill)
            break;
        auto *page = page_at(*storage, last_trap.fault_address);
        if (!page)
            break;
        if (last_trap.fault_address == frame.pc && !(page->flags & page_execute))
        {
            last_trap = {TrapKind::page_fault, 6, 0, frame.pc};
            break;
        }
        fill_tlb(*storage, last_trap.fault_address);
    }
    write_csr<0x41>(0);
    write_csr<0x44>(1);
    ok_loong_execution_active_frame = nullptr;
    executing = false;
    write_csr<0>(saved_crmd);
    return last_trap;
}
extern "C" void ok_loong_execution_trap(uptr is_refill)
{
    const uptr previous_mode = is_refill ? read_csr<0x8f>() : read_csr<1>();
    if ((previous_mode & 3) != 3 || !ok_loong_execution_active_frame)
    {
        auto *uart = reinterpret_cast<volatile u8 *>(0x1fe001e0);
        const char *message = "OK_NATIVE_KERNEL_FAULT loongarch64\n";
        while (*message)
        {
            while (!(uart[5] & 0x20))
            {
            }
            uart[0] = *message++;
        }
        for (;;)
            asm volatile("idle 0");
    }
    auto &frame = *ok_loong_execution_active_frame;
    const uptr status = read_csr<5>();
    const u32 code = (status >> 16) & 0x3f;
    frame.pc = is_refill ? (read_csr<0x8a>() & ~uptr(3)) : read_csr<6>();
    frame.sp = frame.registers[3];
    frame.fs_base = frame.registers[2];
    if (is_refill)
    {
        refill = true;
        last_trap = {TrapKind::page_fault, 0, 0, read_csr<0x89>()};
        write_csr<0x8a>(0);
    }
    else if (code == 0 && (status & (1u << 11)))
        last_trap = {TrapKind::timer, 11, 0, 0};
    else if (code == 11)
    {
        frame.pc += 4;
        last_trap = {TrapKind::syscall, code, 0, 0};
    }
    else
        last_trap = {code >= 1 && code <= 8 ? TrapKind::page_fault : TrapKind::exception, code, status, read_csr<7>()};
}
} // namespace ok::arch::execution
