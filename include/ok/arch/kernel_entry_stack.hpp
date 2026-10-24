#pragma once

#include "ok/user/native_signal_config.hpp"

#include "ok/core/types.hpp"

namespace ok::memory
{
class KernelEntryStackPool;
}

namespace ok::arch::execution
{

// A pool-minted active lease. The canonical pool and its backing must outlive
// every view; copies never own or release storage. Use is serialized on CPU 0.
class KernelEntryStackView final
{
  public:
    constexpr KernelEntryStackView() = default;

    [[nodiscard]] bool valid() const noexcept
    {
        return record_ != nullptr && generation_ != 0 && epoch_ != 0 &&
               record_->state == State::active && record_->generation == generation_ &&
               record_->epoch == epoch_;
    }
    [[nodiscard]] uptr lower() const noexcept { return valid() ? record_->lower : 0; }
    [[nodiscard]] uptr top() const noexcept { return valid() ? record_->top : 0; }
    [[nodiscard]] u64 owner_pid() const noexcept { return valid() ? record_->pid : 0; }
    [[nodiscard]] u64 owner_tid() const noexcept { return valid() ? record_->tid : 0; }
    [[nodiscard]] bool contains(uptr address, usize bytes = 1) const noexcept
    {
        return valid() && address >= record_->lower && address <= record_->top &&
               bytes <= record_->top - address;
    }

  private:
    enum class State : u8 { free, reserved, active, quarantined };
    struct Record
    {
        uptr lower{}, top{};
        u64 pid{}, tid{}, generation{}, epoch{};
        u16 cpu{};
        State state{State::free};
    };

    constexpr KernelEntryStackView(const Record &record, u64 generation, u64 epoch)
        : record_(&record), generation_(generation), epoch_(epoch) {}

    friend class ok::memory::KernelEntryStackPool;
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && defined(__mips64)) || defined(OK_MIPS64_N64_ADMISSION_MODEL)
    // Private observation access changes no exported object fields or layout.
    friend struct Mips64N64PrivateViewAccess;
#endif
#if (defined(OK_NATIVE_SIGNAL_RUNTIME) && (defined(__powerpc__) || defined(__PPC__)) && __SIZEOF_POINTER__ == 4 && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(OK_PPC32_ADMISSION_MODEL)
    friend struct Ppc32PrivateViewAccess;
#endif
    const Record *record_{};
    u64 generation_{}, epoch_{};
};

} // namespace ok::arch::execution
