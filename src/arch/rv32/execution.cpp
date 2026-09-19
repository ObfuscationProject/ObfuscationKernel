#include "ok/arch/execution.hpp"
#include <cstddef>

namespace ok::arch::execution
{
namespace
{
constexpr usize max_pages = 128;
constexpr usize max_leaf_tables = 16;
constexpr u32 pte_valid = 1, pte_read = 2, pte_write = 4, pte_execute = 8, pte_user = 16, pte_accessed = 64,
              pte_dirty = 128;
struct alignas(page_size) Table
{
    std::array<u32, 1024> entries{};
};
struct Space
{
    bool used{false};
    u32 generation{0};
    usize count{0};
    usize leaves{0};
    std::array<PageMapping, max_pages> mappings{};
    Table root;
    std::array<Table, max_leaf_tables> leaf;
};
std::array<Space, max_address_spaces> spaces{};
bool initialized{false};
bool entering{false};

Space *lookup(const AddressSpace &space)
{
    if (space.slot >= spaces.size())
        return nullptr;
    auto &stored = spaces[space.slot];
    return stored.used && stored.generation == space.generation && reinterpret_cast<uptr>(&stored.root) == space.root
               ? &stored
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
u32 table_entry(const Table &table)
{
    return (reinterpret_cast<uptr>(&table) >> 2) | pte_valid;
}
extern "C" void ok_rv32_enter_native(UserFrame *frame, uptr satp, Trap *trap);
static_assert(offsetof(UserFrame, pc) == 128);
static_assert(offsetof(UserFrame, sp) == 132);
static_assert(offsetof(UserFrame, flags) == 136);
static_assert(offsetof(UserFrame, fp_state) == 160);
static_assert(offsetof(Trap, vector) == 4);
static_assert(offsetof(Trap, error_code) == 8);
static_assert(offsetof(Trap, fault_address) == 12);
} // namespace

bool supported() noexcept
{
    return true;
}
Status initialize()
{
    if (initialized)
        return Status::success();
    // The virt kernel remains in M-mode. U-mode uses Sv32 and returns directly
    // to the machine trap vector; no supervisor kernel mapping is exposed.
    uptr hart = 0;
    asm volatile("csrr %0, mhartid" : "=r"(hart));
    if (hart != 0)
        return Status::unsupported("native virt execution currently uses the boot hart");
    const uptr all_memory = ~uptr{0};
    const uptr permission = 0x1f; // PMP NAPOT region, R/W/X; PTEs enforce U access.
    asm volatile("csrw pmpaddr0, %0\ncsrw pmpcfg0, %1" ::"r"(all_memory), "r"(permission) : "memory");
    initialized = true;
    return Status::success();
}
Result<AddressSpace> create_space(std::span<const PageMapping> mappings)
{
    if (!initialized)
        return Status::not_initialized("native execution is not initialized");
    if (mappings.empty() || mappings.size() > max_pages)
        return Status::invalid_argument("invalid user page count");
    for (usize i = 0; i < mappings.size(); ++i)
    {
        const auto &page = mappings[i];
        if (page.virtual_address < user_begin || page.virtual_address >= user_end ||
            (page.virtual_address & (page_size - 1)) != 0 || page.physical_address == 0 ||
            (page.physical_address & (page_size - 1)) != 0 || (page.flags & page_read) == 0 ||
            (page.flags & (page_write | page_execute)) == (page_write | page_execute) ||
            (page.flags & ~(page_read | page_write | page_execute)) != 0 ||
            ((page.flags & page_write) && !(page.flags & page_read)))
            return Status::invalid_argument("invalid Sv32 user mapping");
        for (usize j = 0; j < i; ++j)
            if (mappings[j].virtual_address == page.virtual_address)
                return Status::already_exists("duplicate user page");
    }
    usize slot = 0;
    while (slot < spaces.size() && spaces[slot].used)
        ++slot;
    if (slot == spaces.size())
        return Status::no_memory("native address space table full");
    auto &space = spaces[slot];
    space.root.entries.fill(0);
    for (auto &table : space.leaf)
        table.entries.fill(0);
    space.count = 0;
    space.leaves = 0;
    for (const auto &page : mappings)
    {
        const usize middle_index = page.virtual_address >> 22;
        auto &middle = space.root.entries[middle_index];
        if (middle == 0)
        {
            if (space.leaves == max_leaf_tables)
                return Status::no_memory("Sv32 leaf table capacity exceeded");
            middle = table_entry(space.leaf[space.leaves++]);
        }
        auto *leaf = reinterpret_cast<Table *>((middle >> 10) << 12);
        u32 flags = pte_valid | pte_user | pte_accessed | pte_dirty;
        if (page.flags & page_read)
            flags |= pte_read;
        if (page.flags & page_write)
            flags |= pte_write;
        if (page.flags & page_execute)
            flags |= pte_execute;
        leaf->entries[(page.virtual_address >> 12) & 1023u] = (page.physical_address >> 2) | flags;
        space.mappings[space.count++] = page;
    }
    if (++space.generation == 0)
        ++space.generation;
    space.used = true;
    asm volatile("fence rw,rw\nfence.i" ::: "memory");
    return AddressSpace{
        .root = reinterpret_cast<uptr>(&space.root), .slot = static_cast<u32>(slot), .generation = space.generation};
}
Status destroy_space(AddressSpace &space)
{
    if (entering)
        return Status::busy("cannot destroy an executing address space");
    auto *stored = lookup(space);
    if (stored == nullptr)
        return Status::invalid_argument("stale native address space");
    stored->used = false;
    stored->count = 0;
    space = {};
    return Status::success();
}
UserFrame initial_frame(uptr entry, uptr stack)
{
    UserFrame frame{};
    frame.pc = entry;
    frame.sp = stack;
    frame.registers[2] = stack;
    return frame;
}
Result<Trap> enter(AddressSpace &space, UserFrame &frame)
{
    auto *stored = lookup(space);
    if (!initialized || stored == nullptr)
        return Status::not_initialized("native address space is unavailable");
    if (entering)
        return Status::busy("nested native entry is unsupported");
    const auto *entry = mapping(*stored, frame.pc);
    if (entry == nullptr || !(entry->flags & page_execute) || (frame.pc & 1) != 0 || frame.sp <= user_begin ||
        frame.sp > user_end || !user_range(space, frame.sp - 1, 1, true))
        return Status::invalid_argument("invalid user entry frame");
    entering = true;
    Trap trap{};
    const uptr satp = (uptr{1} << 31) | (space.root >> 12);
    ok_rv32_enter_native(&frame, satp, &trap);
    entering = false;
    return trap;
}
Syscall syscall(const UserFrame &frame)
{
    Syscall call{.number = frame.registers[17]};
    for (usize i = 0; i < call.arguments.size(); ++i)
        call.arguments[i] = frame.registers[10 + i];
    return call;
}
void set_return_value(UserFrame &frame, i64 value)
{
    frame.registers[10] = static_cast<uptr>(value);
}
bool user_range(const AddressSpace &space, uptr address, usize size, bool write)
{
    const auto *stored = lookup(space);
    if (stored == nullptr)
        return false;
    if (size == 0)
        return true;
    if (address < user_begin || address >= user_end || size > user_end - address)
        return false;
    for (uptr cursor = address; cursor < address + size;)
    {
        const auto *page = mapping(*stored, cursor);
        if (page == nullptr || !(page->flags & (write ? page_write : page_read)))
            return false;
        const auto next = (cursor & ~(page_size - 1)) + page_size;
        cursor = next < address + size ? next : address + size;
    }
    return true;
}
Status copy_from_user(const AddressSpace &space, void *destination, uptr source, usize size)
{
    if ((destination == nullptr && size != 0) || !user_range(space, source, size))
        return Status::fault("invalid user read range");
    const auto *stored = lookup(space);
    auto *output = static_cast<std::byte *>(destination);
    for (usize i = 0; i < size;)
    {
        const auto *page = mapping(*stored, source + i);
        const auto offset = (source + i) & (page_size - 1);
        const usize chunk = size - i < page_size - offset ? size - i : page_size - offset;
        const auto *input = reinterpret_cast<const std::byte *>(page->physical_address + offset);
        for (usize j = 0; j < chunk; ++j)
            output[i + j] = input[j];
        i += chunk;
    }
    return Status::success();
}
Status copy_to_user(const AddressSpace &space, uptr destination, const void *source, usize size)
{
    if ((source == nullptr && size != 0) || !user_range(space, destination, size, true))
        return Status::fault("invalid user write range");
    const auto *stored = lookup(space);
    const auto *input = static_cast<const std::byte *>(source);
    for (usize i = 0; i < size;)
    {
        const auto *page = mapping(*stored, destination + i);
        const auto offset = (destination + i) & (page_size - 1);
        const usize chunk = size - i < page_size - offset ? size - i : page_size - offset;
        auto *output = reinterpret_cast<std::byte *>(page->physical_address + offset);
        for (usize j = 0; j < chunk; ++j)
            output[j] = input[i + j];
        i += chunk;
    }
    return Status::success();
}
} // namespace ok::arch::execution
