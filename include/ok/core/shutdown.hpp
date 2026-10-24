#pragma once

#include "ok/core/power.hpp"
#include <array>

namespace ok
{
// Kernel-internal facts, captured from the original native init by its runtime.
// The runtime must mint a nonzero incarnation that never repeats while a ticket
// may exist, including reconstruction at the same address. PID is not identity.
struct ShutdownOwner
{
    const void *runtime{};
    u64 incarnation{};
    u64 process{};
    u64 leader{};
    bool operator==(const ShutdownOwner &) const = default;
};

struct ShutdownCaller
{
    ShutdownOwner owner{};
    u64 thread{};
    usize live_threads{};
    bool live{};
    std::array<u32, 6> ids{}; // Real, effective and saved UID, then GID.
};

enum class ShutdownPhase : u8
{
    idle,
    preflighting,
    prepared,
    queued,
    checking_boundary,
    revalidating,
    closing_admission,
    stopping_producers,
    retiring_native,
    draining_resources,
    sealing_storage,
    ready,
    dispatched,
    failed,
};

struct ShutdownTicket
{
    const void *coordinator{};
    ShutdownOwner owner{};
    u64 generation{};
    SystemPowerAction action{SystemPowerAction::none};
    bool operator==(const ShutdownTicket &) const = default;
};

// Every operation is bounded and serialized on the boot CPU. Preflight and
// check_commit are read-only. Cleanup operations consume their owners once;
// failure never permits retry or ordinary work. The adapter must outlive us.
// check_outer_boundary must establish outer-loop provenance, not merely an
// inactive native entry slot (syscall dispatch also runs on the service stack).
template <typename Services>
concept ShutdownServices = requires(Services &services, SystemPowerAction action, ShutdownOwner owner)
{
    { services.preflight(action) } -> std::same_as<Status>;
    { services.check_commit(owner) } -> std::same_as<Status>;
    { services.check_outer_boundary() } -> std::same_as<Status>;
    { services.close_admission() } -> std::same_as<Status>;
    { services.stop_producers() } -> std::same_as<Status>;
    { services.retire_native() } -> std::same_as<Status>;
    { services.drain_resources() } -> std::same_as<Status>;
    { services.seal_storage() } -> std::same_as<Status>;
};

// Mechanism only: an adapter must prove native retirement and the original
// storage owner's clean seal. A successful model adapter is not that proof.
template <ShutdownServices Services> class ShutdownCoordinator final
{
  public:
    constexpr explicit ShutdownCoordinator(Services &services) : services_(services) {}
    ShutdownCoordinator(const ShutdownCoordinator &) = delete;
    ShutdownCoordinator &operator=(const ShutdownCoordinator &) = delete;
    ShutdownCoordinator(ShutdownCoordinator &&) = delete;
    ShutdownCoordinator &operator=(ShutdownCoordinator &&) = delete;

    Status bind_original_init(ShutdownOwner owner)
    {
        if (active_) return Status::busy("shutdown callback is active");
        if (owner_.runtime || phase_ != ShutdownPhase::idle || !owner.runtime ||
            !owner.incarnation || owner.process != 1 || !owner.leader)
            return Status::denied("shutdown requires a one-shot original init identity");
        owner_ = owner;
        return {};
    }

    Result<ShutdownTicket> prepare(const ShutdownCaller &caller, SystemPowerAction action)
    {
        if (active_) return Status::busy("shutdown callback is active");
        if (auto status = authorize(caller); !status) return status;
        if (!valid_action(action)) return Status::invalid_argument("invalid shutdown action");
        if (phase_ == ShutdownPhase::failed) return failure_;
        if (phase_ == ShutdownPhase::prepared && ticket_.action == action) return ticket_;
        if (phase_ != ShutdownPhase::idle) return Status::busy("shutdown is already reserved");
        Guard guard{active_};
        phase_ = ShutdownPhase::preflighting;
        const auto status = services_.preflight(action);
        if (!status)
        {
            phase_ = ShutdownPhase::idle;
            return status;
        }
        // This coordinator has one boot lifetime and no reset or cancellation.
        ticket_ = {this, owner_, 1, action};
        phase_ = ShutdownPhase::prepared;
        return ticket_;
    }

    Status commit(const ShutdownCaller &caller, ShutdownTicket ticket)
    {
        if (active_) return Status::busy("shutdown callback is active");
        if (auto status = authorize(caller); !status) return status;
        if (auto status = validate(ticket); !status) return status;
        if (phase_ == ShutdownPhase::failed) return failure_;
        if (phase_ == ShutdownPhase::queued) return {}; // Idempotent acknowledgement; no cleanup.
        if (phase_ != ShutdownPhase::prepared) return Status::busy("shutdown ticket was already committed");
        Guard guard{active_};
        if (auto status = services_.check_commit(owner_); !status) return status;
        phase_ = ShutdownPhase::queued;
        return {}; // Never retire the committing caller on its syscall stack.
    }

    Status advance()
    {
        if (active_) return Status::busy("shutdown callback is active");
        if (phase_ == ShutdownPhase::failed) return failure_;
        if (phase_ != ShutdownPhase::queued) return Status::busy("shutdown is not queued");
        Guard guard{active_};
        phase_ = ShutdownPhase::checking_boundary;
        if (auto status = services_.check_outer_boundary(); !status) return fail(status);
        phase_ = ShutdownPhase::revalidating;
        if (auto status = services_.preflight(ticket_.action); !status) return fail(status);
        phase_ = ShutdownPhase::closing_admission;
        barrier_started_ = true;
        if (auto status = services_.close_admission(); !status) return fail(status);
        phase_ = ShutdownPhase::stopping_producers;
        if (auto status = services_.stop_producers(); !status) return fail(status);
        phase_ = ShutdownPhase::retiring_native;
        if (auto status = services_.retire_native(); !status) return fail(status);
        phase_ = ShutdownPhase::draining_resources;
        if (auto status = services_.drain_resources(); !status) return fail(status);
        phase_ = ShutdownPhase::sealing_storage;
        if (auto status = services_.seal_storage(); !status) return fail(status);
        phase_ = ShutdownPhase::ready;
        return {};
    }

    Result<SystemPowerAction> consume_action()
    {
        if (active_) return Status::busy("shutdown callback is active");
        if (phase_ == ShutdownPhase::failed) return failure_;
        if (phase_ != ShutdownPhase::ready) return Status::busy("shutdown has no clean action");
        Guard guard{active_};
        if (auto status = services_.check_outer_boundary(); !status) return fail(status);
        phase_ = ShutdownPhase::dispatched;
        return ticket_.action;
    }

    [[nodiscard]] ShutdownPhase phase() const { return phase_; }
    [[nodiscard]] ShutdownPhase failed_at() const { return failed_at_; }
    [[nodiscard]] Status failure() const { return failure_; }
    [[nodiscard]] bool barrier_started() const { return barrier_started_; }
    [[nodiscard]] SystemPowerAction ready_action() const
    {
        return phase_ == ShutdownPhase::ready ? ticket_.action : SystemPowerAction::none;
    }
    [[nodiscard]] bool must_hold() const
    {
        return phase_ != ShutdownPhase::idle && phase_ != ShutdownPhase::preflighting &&
               phase_ != ShutdownPhase::prepared;
    }

    static constexpr bool valid_action(SystemPowerAction action)
    {
        return action == SystemPowerAction::halt || action == SystemPowerAction::reboot ||
               action == SystemPowerAction::poweroff;
    }

  private:
    struct Guard
    {
        bool &active;
        explicit Guard(bool &value) : active(value) { active = true; }
        ~Guard() { active = false; }
    };
    Status authorize(const ShutdownCaller &caller) const
    {
        if (!owner_.runtime || caller.owner != owner_ || !caller.live ||
            caller.thread != owner_.leader || caller.live_threads != 1)
            return Status::denied("shutdown requires the original single-threaded init");
        for (auto id : caller.ids)
            if (id) return Status::denied("shutdown requires all six root identities");
        return {};
    }
    Status validate(ShutdownTicket ticket) const
    {
        if (!ticket_.generation || ticket != ticket_)
            return Status::denied("shutdown ticket is foreign or stale");
        return {};
    }
    Status fail(Status status)
    {
        failed_at_ = phase_;
        failure_ = status;
        phase_ = ShutdownPhase::failed;
        return status;
    }

    Services &services_;
    ShutdownOwner owner_{};
    ShutdownTicket ticket_{};
    ShutdownPhase phase_{ShutdownPhase::idle};
    ShutdownPhase failed_at_{ShutdownPhase::idle};
    Status failure_{};
    bool active_{};
    bool barrier_started_{};
};
} // namespace ok
