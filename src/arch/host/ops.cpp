#include "../ops_private.hpp"

#include <atomic>

namespace ok::arch::detail {
namespace {

class HostOperations final : public ArchOperationsBase {
public:
    HostOperations() : ArchOperationsBase(Architecture::host) {}

    [[nodiscard]] std::string_view name() const override { return "host"; }
    [[nodiscard]] usize page_size() const override { return 4096; }
    [[nodiscard]] usize register_count() const override { return 16; }
    [[nodiscard]] Endianness endianness() const override { return Endianness::little; }
    [[nodiscard]] bool supports_user_mode() const override { return false; }
    [[nodiscard]] std::string_view interrupt_model() const override { return "host-simulated"; }
    [[nodiscard]] std::string_view syscall_model() const override { return "host-simulated"; }
    [[nodiscard]] std::string_view user_transition_model() const override { return "host-simulated"; }
    [[nodiscard]] u64 read_cycle_counter() const noexcept override { return fallback_cycle_counter(); }
    void memory_fence() noexcept override { std::atomic_thread_fence(std::memory_order_seq_cst); }
};

} // namespace

ArchOperations& host_operations()
{
    static HostOperations operations;
    return operations;
}

} // namespace ok::arch::detail
