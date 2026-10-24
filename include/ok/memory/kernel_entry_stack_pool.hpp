#pragma once

#include "ok/user/native_signal_config.hpp"

#include "ok/arch/kernel_entry_stack.hpp"
#include "ok/core/types.hpp"

#include <array>
#include <span>

namespace ok::arch::execution { struct Ppc32PrivateViewAccess; }

namespace ok::memory
{

class KernelEntryStackPool;

struct KernelEntryStackOwner
{
    u64 pid{}, tid{};
    [[nodiscard]] constexpr bool operator==(const KernelEntryStackOwner &) const = default;
};

struct KernelEntryStackHandle
{
    const KernelEntryStackPool *pool{};
    u32 slot{};
    u64 generation{};
    [[nodiscard]] constexpr bool operator==(const KernelEntryStackHandle &) const = default;
};

struct KernelEntryStackAccounting
{
    usize leased_slots{}, active_slots{}, quarantined_slots{}, peak_slots{};
    usize backing_bytes{}, leased_usable_bytes{};
    u64 reservations{}, releases{}, activations{};
};

// Canonical, serialized boot-CPU ownership. Backing is supplied by trusted
// platform code and must remain supervisor-accessible for the pool's lifetime.
// Canary pages are mapped diagnostic storage, not faulting guard pages.
class KernelEntryStackPool final : private NonCopyable
{
  public:
    static constexpr usize capacity = 128;
    static constexpr usize page_size = 4096;
    static constexpr usize usable_bytes = 65536;
    static constexpr usize canary_bytes = page_size;
    static constexpr usize slot_stride = usable_bytes + 2 * canary_bytes;
    static constexpr usize arena_bytes = capacity * slot_stride;

    Status activate(std::span<std::byte> backing)
    {
        if (initialized_)
            return Status::already_exists("entry stack pool is already initialized");
        const uptr begin = reinterpret_cast<uptr>(backing.data());
        const uptr self = reinterpret_cast<uptr>(this);
        if (begin == 0 || backing.size() != arena_bytes || begin % page_size != 0 ||
            arena_bytes > ~uptr{0} - begin)
            return Status::invalid_argument("entry stack arena bounds or alignment are invalid");
        if (begin >= self ? begin - self < sizeof(*this) : self - begin < backing.size())
            return Status::invalid_argument("entry stack arena overlaps ownership metadata");
        backing_ = backing;
        for (auto &byte : backing_)
            byte = std::byte{};
        initialized_ = true;
        return Status::success();
    }

    Result<KernelEntryStackHandle> reserve(KernelEntryStackOwner owner)
    {
        if (!initialized_)
            return Status::not_initialized("entry stack arena is not initialized");
        if (!fault_)
            return fault_;
        if (owner.pid == 0 || owner.tid == 0)
            return Status::invalid_argument("entry stack owner must name a process and thread");
        for (const auto &record : records_)
            if (record.state != State::free && record.pid == owner.pid && record.tid == owner.tid)
                return Status::already_exists("thread already owns an entry stack lease");
        if (generation_ == ~u64{0})
            return Status::overflow("entry stack lease generation exhausted");
        for (usize slot = 0; slot < capacity; ++slot)
        {
            auto &record = records_[slot];
            if (record.state != State::free)
                continue;
            const uptr lower = reinterpret_cast<uptr>(backing_.data() + slot * slot_stride + canary_bytes);
            record = {.lower = lower, .top = lower + usable_bytes, .pid = owner.pid, .tid = owner.tid,
                      .generation = ++generation_, .state = State::reserved};
            set_canaries(slot, record.generation);
            ++leased_;
            if (leased_ > peak_)
                peak_ = leased_;
            return KernelEntryStackHandle{this, static_cast<u32>(slot), record.generation};
        }
        return Status::no_memory("entry stack lease capacity exhausted");
    }

    Result<arch::execution::KernelEntryStackView> activate_entry(
        KernelEntryStackHandle handle, KernelEntryStackOwner owner, u16 cpu)
    {
        if (auto status = validate(handle, owner); !status)
            return status;
        if (!fault_)
            return fault_;
        if (cpu != 0)
            return Status::unsupported("entry stack execution is limited to the boot CPU");
        if (active_slot_ != capacity)
            return Status::busy("another entry stack lease is active");
        if (epoch_ == ~u64{0})
            return Status::overflow("entry stack activation epoch exhausted");
        auto &record = records_[handle.slot];
        if (auto status = check_canaries(handle.slot); !status)
            return quarantine(record, status);
        record.state = State::active;
        record.cpu = cpu;
        record.epoch = ++epoch_;
        active_slot_ = handle.slot;
        return arch::execution::KernelEntryStackView{record, record.generation, record.epoch};
    }

    Status deactivate_entry(KernelEntryStackHandle handle, KernelEntryStackOwner owner, u16 cpu)
    {
        if (auto status = validate(handle, owner); !status)
            return status;
        auto &record = records_[handle.slot];
        if (record.state != State::active || active_slot_ != handle.slot)
            return Status::not_found("entry stack lease is not active");
        if (cpu != record.cpu)
            return Status::denied("entry stack activation belongs to another CPU");
        // Revoke every copied view even when a damaged canary prevents reuse.
        record.state = State::reserved;
        active_slot_ = capacity;
        if (auto status = check_canaries(handle.slot); !status)
            return quarantine(record, status);
        return Status::success();
    }

    Status release(KernelEntryStackHandle handle, KernelEntryStackOwner owner)
    {
        if (auto status = validate_inactive(handle, owner); !status)
            return status;
        auto &record = records_[handle.slot];
        auto bytes = backing_.subspan(handle.slot * slot_stride, slot_stride);
        for (auto &byte : bytes)
            byte = std::byte{};
        record = {};
        --leased_;
        ++releases_;
        return Status::success();
    }

    // Exec can preflight ownership before escrowing handles across replacement;
    // this never changes healthy backing, lease generation or accounting.
    Status validate_inactive(KernelEntryStackHandle handle, KernelEntryStackOwner owner)
    {
        if (auto status = validate(handle, owner); !status)
            return status;
        auto &record = records_[handle.slot];
        if (record.state == State::active)
            return Status::busy("entry stack lease is still active");
        if (auto status = check_canaries(handle.slot); !status)
            return quarantine(record, status);
        return Status::success();
    }

    Result<uptr> top(KernelEntryStackHandle handle, KernelEntryStackOwner owner)
    {
        if (auto status = validate_inactive(handle, owner); !status)
            return status;
        return records_[handle.slot].top;
    }

    Result<u64> inactive_epoch(KernelEntryStackHandle handle, KernelEntryStackOwner owner)
    {
        if (auto status = validate_inactive(handle, owner); !status) return status;
        return records_[handle.slot].epoch;
    }

    // Compare the private record pointer, not merely matching owner/bounds.
    // Equal leases and owners in another pool do not authorize this view.
    Result<u64> active_epoch(KernelEntryStackHandle handle, KernelEntryStackOwner owner,
                             const arch::execution::KernelEntryStackView &view) const
    {
        if (auto status = validate(handle, owner); !status) return status;
        const auto &record = records_[handle.slot];
        if (active_slot_ != handle.slot || record.cpu != 0 || !view.valid() ||
            view.record_ != &record || view.generation_ != handle.generation ||
            view.epoch_ != record.epoch)
            return Status::denied("entry view does not name the exact active pool lease");
        return record.epoch;
    }

    [[nodiscard]] KernelEntryStackAccounting accounting() const
    {
        return {.leased_slots = leased_, .active_slots = active_slot_ != capacity ? 1u : 0u,
                .quarantined_slots = quarantined_, .peak_slots = peak_,
                .backing_bytes = initialized_ ? arena_bytes : 0, .leased_usable_bytes = leased_ * usable_bytes,
                .reservations = generation_, .releases = releases_, .activations = epoch_};
    }

  private:
    using View = arch::execution::KernelEntryStackView;
    using State = View::State;
    using Record = View::Record;

    [[nodiscard]] Status validate(KernelEntryStackHandle handle, KernelEntryStackOwner owner) const
    {
        if (!initialized_)
            return Status::not_initialized("entry stack arena is not initialized");
        if (handle.pool != this || handle.slot >= capacity || handle.generation == 0)
            return Status::invalid_argument("entry stack handle identity is invalid");
        const auto &record = records_[handle.slot];
        if (record.state == State::free || record.generation != handle.generation)
            return Status::not_found("entry stack handle is stale");
        if (owner.pid != record.pid || owner.tid != record.tid)
            return Status::denied("entry stack handle belongs to another thread");
        if (record.state == State::quarantined)
            return Status::fault("entry stack lease is quarantined");
        return Status::success();
    }
    static std::byte canary(usize slot, u64 generation, usize offset)
    {
        return std::byte{static_cast<u8>(0xa5u ^ static_cast<u8>(slot) ^
               static_cast<u8>(generation >> ((offset % 8) * 8)) ^ static_cast<u8>(offset))};
    }
    void set_canaries(usize slot, u64 generation)
    {
        auto bytes = backing_.subspan(slot * slot_stride, slot_stride);
        for (usize i = 0; i < canary_bytes; ++i)
        {
            bytes[i] = canary(slot, generation, i);
            bytes[canary_bytes + usable_bytes + i] = canary(slot, generation, i + canary_bytes);
        }
    }
    [[nodiscard]] Status check_canaries(usize slot) const
    {
        const auto bytes = backing_.subspan(slot * slot_stride, slot_stride);
        const u64 generation = records_[slot].generation;
        for (usize i = 0; i < canary_bytes; ++i)
            if (bytes[i] != canary(slot, generation, i) ||
                bytes[canary_bytes + usable_bytes + i] != canary(slot, generation, i + canary_bytes))
                return Status::fault("entry stack canary corruption detected");
        return Status::success();
    }
    Status quarantine(Record &record, Status status)
    {
        record.state = State::quarantined;
        ++quarantined_;
        if (fault_)
            fault_ = status;
        return status;
    }

#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__powerpc__) || defined(__PPC__)) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(OK_PPC32_ADMISSION_MODEL)
    friend struct arch::execution::Ppc32PrivateViewAccess;
#endif
    friend struct KernelEntryStackPoolTestAccess;
    std::span<std::byte> backing_{};
    std::array<Record, capacity> records_{};
    usize leased_{}, peak_{}, quarantined_{}, active_slot_{capacity};
    u64 generation_{}, releases_{}, epoch_{};
    Status fault_{};
    bool initialized_{};
};

} // namespace ok::memory
