#include "ok/memory/runtime_frames.hpp"

namespace ok::memory
{
namespace
{
u64 next_allocator_identity()
{
    static u64 counter = 0x52464131; // 'RFA1'
    return ++counter;
}

[[nodiscard]] constexpr usize words_for_bits(usize bits)
{
    return (bits + 63) / 64;
}
} // namespace

usize RuntimeFrameAllocator::metadata_bytes_required(const PhysicalMemoryMap &map, usize page_size)
{
    if (!map.normalized() || page_size == 0)
        return 0;
    const usize frames = map.usable_pages(page_size);
    const usize ranges = map.usable().size();
    return words_for_bits(frames) * sizeof(u64) + frames * sizeof(u64) + ranges * sizeof(Range) + alignof(u64);
}

Status RuntimeFrameAllocator::initialize(const PhysicalMemoryMap &map, usize page_size, std::span<std::byte> metadata)
{
    if (frame_count_ != 0)
        return Status{StatusCode::busy, "runtime frame allocator already initialised"};
    if (!map.normalized())
        return Status{StatusCode::not_initialized, "physical memory map is not normalised"};
    if (page_size == 0 || (page_size & (page_size - 1)) != 0)
        return Status{StatusCode::invalid_argument, "page size must be a power of two"};
    const usize frames = map.usable_pages(page_size);
    if (frames == 0)
        return Status{StatusCode::no_memory, "no usable frames"};
    const usize required = metadata_bytes_required(map, page_size);
    if (metadata.size() < required)
        return Status{StatusCode::no_memory, "frame allocator metadata storage too small"};
    // Align the metadata start to u64.
    uptr start = reinterpret_cast<uptr>(metadata.data());
    const uptr aligned = (start + alignof(u64) - 1) & ~static_cast<uptr>(alignof(u64) - 1);
    std::byte *cursor = metadata.data() + (aligned - start);
    const usize words = words_for_bits(frames);
    bitmap_ = {reinterpret_cast<u64 *>(cursor), words};
    cursor += words * sizeof(u64);
    generations_ = {reinterpret_cast<u64 *>(cursor), frames};
    cursor += frames * sizeof(u64);
    ranges_ = {reinterpret_cast<Range *>(cursor), map.usable().size()};
    for (auto &w : bitmap_)
        w = 0;
    for (auto &g : generations_)
        g = 0;
    usize index = 0;
    range_count_ = 0;
    for (const auto &region : map.usable())
    {
        const usize count = region.size / page_size;
        if (count == 0)
            continue;
        ranges_[range_count_++] = {region.base, index, count};
        index += count;
    }
    page_size_ = page_size;
    frame_count_ = index;
    used_count_ = 0;
    next_free_hint_ = 0;
    identity_ = next_allocator_identity();
    return Status::success();
}

bool RuntimeFrameAllocator::bit(usize index) const
{
    return (bitmap_[index / 64] >> (index % 64)) & 1u;
}

void RuntimeFrameAllocator::set_bit(usize index, bool value)
{
    const u64 mask = u64{1} << (index % 64);
    if (value)
        bitmap_[index / 64] |= mask;
    else
        bitmap_[index / 64] &= ~mask;
}

bool RuntimeFrameAllocator::address_for(usize index, uptr &address) const
{
    for (usize r = 0; r < range_count_; ++r)
    {
        const auto &range = ranges_[r];
        if (index >= range.first_index && index < range.first_index + range.count)
        {
            address = range.base + static_cast<uptr>(index - range.first_index) * page_size_;
            return true;
        }
    }
    return false;
}

Result<RuntimeFrame> RuntimeFrameAllocator::allocate(ChargeAccount &account)
{
    if (frame_count_ == 0)
        return Status{StatusCode::not_initialized, "runtime frame allocator not initialised"};
    if (used_count_ == frame_count_)
        return Status{StatusCode::no_memory, "physical frames exhausted"};
    if (next_generation_ == ~u64{0})
        return Status{StatusCode::overflow, "frame generation exhausted"};
    if (auto charged = account.charge(1); !charged.ok())
        return charged;
    // Word-wise scan from the hint.
    const usize words = bitmap_.size();
    for (usize scanned = 0; scanned < words; ++scanned)
    {
        const usize w = (next_free_hint_ / 64 + scanned) % words;
        if (bitmap_[w] == ~u64{0})
            continue;
        for (usize b = 0; b < 64; ++b)
        {
            const usize index = w * 64 + b;
            if (index >= frame_count_)
                break;
            if (bit(index))
                continue;
            set_bit(index, true);
            ++used_count_;
            next_free_hint_ = index + 1;
            uptr address{};
            if (!address_for(index, address))
                return Status{StatusCode::fault, "frame index outside every range"};
            generations_[index] = next_generation_++;
            return RuntimeFrame{address, index, identity_, generations_[index]};
        }
    }
    account.uncharge(1);
    return Status{StatusCode::no_memory, "bitmap scan found no free frame"};
}

Status RuntimeFrameAllocator::release(RuntimeFrame frame, ChargeAccount &account)
{
    if (frame_count_ == 0)
        return Status{StatusCode::not_initialized, "runtime frame allocator not initialised"};
    if (frame.owner != identity_ || frame.index >= frame_count_ || !bit(frame.index) ||
        generations_[frame.index] != frame.generation)
        return Status{StatusCode::invalid_argument, "foreign, stale or free frame handle"};
    uptr address{};
    if (!address_for(frame.index, address) || address != frame.address)
        return Status{StatusCode::invalid_argument, "frame address does not match its index"};
    set_bit(frame.index, false);
    generations_[frame.index] = 0;
    --used_count_;
    if (frame.index < next_free_hint_)
        next_free_hint_ = frame.index;
    account.uncharge(1);
    return Status::success();
}

} // namespace ok::memory
