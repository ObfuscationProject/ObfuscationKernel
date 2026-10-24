#pragma once

#include "ok/fs/ext4_allocated_empty_data_preview.hpp"
#include "ok/fs/ext4_reclaim_history.hpp"
#include <type_traits>

namespace ok::fs
{
struct Ext4AllocatedEmptyDataReadCapacity
{
    block::SessionCounter reads{};
    u32 groups{};
};
enum class Ext4AllocatedEmptyDataSessionPreviewPhase : u8 { unused, preparing, ready, failed };
struct Ext4AllocatedEmptyDataSessionPreviewReport
{
    Ext4AllocatedEmptyDataSessionPreviewPhase phase{Ext4AllocatedEmptyDataSessionPreviewPhase::unused};
    Status status{};
    Ext4AllocatedEmptyDataReadCapacity reserved{};
    Ext4AllocatedEmptyDataPreviewReport preview{};
    block::SessionReport initial{}, observed{};
    block::SessionCounter attempts{}, completions{};
    bool dispatch_pending{};
};

// Own this off-stack object for the exact original Session's excluded-writer
// lifetime. It performs only guarded READs; no report or reservation grants
// publication, checkpoint, FLUSH or WRITE authority. This is not a mounted owner.
template <typename Session>
class Ext4AllocatedEmptyDataSessionPreview final
{
    template <typename> friend class Ext4AllocatedEmptyDataTransaction;
    using Phase = Ext4AllocatedEmptyDataSessionPreviewPhase;
    struct Reader
    {
        Ext4AllocatedEmptyDataSessionPreview &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> out)
        { return owner.read(number, out); }
    };
    struct Dispatch
    {
        Ext4AllocatedEmptyDataSessionPreview &owner;
        [[nodiscard]] Status in_flight_exact(Session &session)
        { return owner.in_flight_exact(session); }
    };
    struct Control
    {
        Phase phase{Phase::unused};
        block::SessionCounter attempts{}, completions{};
        u32 groups{};
        u64 sector{};
        bool pending{};
    };
  public:
    Ext4AllocatedEmptyDataSessionPreview() = default;
    ~Ext4AllocatedEmptyDataSessionPreview()
    {
        if (control_.pending || sealed_.pending)
            static_cast<void>(anchor_.abort(Status::interrupted("allocated empty data preview unfinished READ")));
    }
    Ext4AllocatedEmptyDataSessionPreview(const Ext4AllocatedEmptyDataSessionPreview &) = delete;
    Ext4AllocatedEmptyDataSessionPreview &operator=(const Ext4AllocatedEmptyDataSessionPreview &) = delete;
    Ext4AllocatedEmptyDataSessionPreview(Ext4AllocatedEmptyDataSessionPreview &&) = delete;
    Ext4AllocatedEmptyDataSessionPreview &operator=(Ext4AllocatedEmptyDataSessionPreview &&) = delete;

    template <block::SessionCounterPolicy Counter = block::SessionCounter>
    [[nodiscard]] static Result<Ext4AllocatedEmptyDataReadCapacity> capacity(
        const block::SessionReport &initial, u32 groups)
    {
        if (!groups || groups > Ext4RecoveryInfo::group_capacity)
            return Status::invalid_argument("allocated empty data preview group capacity");
        if (!stable(initial))
            return Status::fault("allocated empty data preview requires a stable original Session");
        // A checked inode decode reads at most D=2+4G blocks. Each old/final
        // view opens with 2+2D+1024 reads and proves ownership with
        // 2D+G+128(D+1)+2+2G reads. Old candidate proof adds eight
        // metadata reads; initial/final capture adds 2*14, including data.
        // Overlay hits cost no Session READs; counting them is conservative.
        const block::SessionCounter reads = 2876 + 1062 * block::SessionCounter(groups);
        if (!block::session_counter_fits<Counter>(initial.reads, reads) ||
            !block::session_counter_fits<Counter>(initial.completed_reads, reads))
            return Status::overflow("allocated empty data preview whole READ capacity exhausted");
        return Ext4AllocatedEmptyDataReadCapacity{reads, groups};
    }

    [[nodiscard]] Status prepare(Session &session, const Ext4RecoveryInfo &binding)
    {
        if (attempted_) return Status::already_exists("allocated empty data Session preview is one-shot");
        // Refuse borrowing aliases before changing any potentially aliased byte.
        if (overlap(this, sizeof(*this), &session, sizeof(session)) ||
            overlap(this, sizeof(*this), &binding, sizeof(binding)))
            return Status::invalid_argument("allocated empty data Session preview input overlap");
        attempted_ = true;
        if (auto status = anchor_.bind(session); !status) return refuse(status);
        original_ = original_seal_ = &session;
        if constexpr (requires { session.observe_binding(); })
        {
            const auto binding_observation = session.observe_binding();
            if (!binding_observation) return refuse(binding_observation.status());
            original_binding_ = original_binding_seal_ = binding_observation.value();
        }
        const auto actual = session.report();
        if (!actual) return refuse(actual.status());
        observed_ = actual.value();
        if (!binding.metadata_count || binding.metadata_count % 4 ||
            binding.metadata_count > binding.metadata.size() || !binding.structural_metadata().valid())
            return refuse(Status::invalid_argument("allocated empty data Session preview metadata domain"));
        const auto reserved = capacity(actual.value(), u32(binding.metadata_count / 4));
        if (!reserved) return refuse(reserved.status());
        const auto geometry = session.geometry();
        const auto bounds = session.bounds();
        const auto transfer = session.transfer_limit();
        if (!transfer) return refuse(transfer.status());
        if (geometry.block_size != 512 || !geometry.block_count || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || transfer.value() < 1024 ||
            binding.layout.block_size != 1024 || !binding.layout.filesystem_blocks ||
            binding.layout.first_sector >= geometry.block_count ||
            binding.layout.filesystem_blocks > (geometry.block_count - binding.layout.first_sector) / 2)
            return refuse(Status::unsupported("allocated empty data Session preview volume geometry"));
        initial_ = initial_seal_ = progress_ = progress_seal_ = actual.value();
        geometry_ = geometry_seal_ = geometry; bounds_ = bounds_seal_ = bounds;
        binding_ = binding_seal_ = binding;
        reserved_ = reserved.value();
        control_.groups = reserved_.groups; control_.phase = Phase::preparing; sync();
        if (auto status = preview_.prepare(reader_, binding_, workspace_); !status) return reject(status);
        if (auto status = idle_exact(); !status) return status;
        if (preview_.report().read_attempts != control_.attempts ||
            preview_.report().completed_reads != control_.completions)
            return reject(Status::fault("allocated empty data preview and Session READ totals differ"));
        control_.phase = Phase::ready; sync(); return {};
    }

    [[nodiscard]] Ext4AllocatedEmptyDataSessionPreviewReport report() const
    {
        return {control_.phase, status_, reserved_, preview_.report(), initial_, observed_,
                control_.attempts, control_.completions, control_.pending};
    }
  private:
    static bool overlap(const void *a, usize a_size, const void *b, usize b_size)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < a_size : one - two < b_size;
    }
    static bool stable(const block::SessionReport &value)
    {
        return value.state == block::DeviceState::owned && value.first_error == StatusCode::ok &&
            value.identity && value.registration_generation && value.session_serial &&
            !value.in_flight && !value.pending_writes && value.reads == value.completed_reads &&
            value.writes == value.completed_writes && value.flushes == value.completed_flushes;
    }
    static bool same_geometry(driver::BlockGeometry a, driver::BlockGeometry b)
    { return a.block_size == b.block_size && a.block_count == b.block_count && a.writable == b.writable; }
    static bool same_bounds(block::SessionBounds a, block::SessionBounds b)
    { return a.first == b.first && a.count == b.count; }
    static bool same_control(const Control &a, const Control &b)
    {
        return a.phase == b.phase && a.attempts == b.attempts && a.completions == b.completions &&
            a.groups == b.groups && a.sector == b.sector && a.pending == b.pending;
    }
    void sync() { sealed_ = control_; }
    [[nodiscard]] Status bind_owner_guard(void *context, Status (*check)(void *))
    {
        if (attempted_ || owner_guard_retired_ || owner_context_ || owner_check_ || !context || !check)
            return Status::already_exists("recovery preview owner attachment is one-shot");
        owner_context_ = owner_context_seal_ = context;
        owner_check_ = owner_check_seal_ = check;
        return {};
    }
    void retire_owner_guard()
    {
        owner_context_ = owner_context_seal_ = nullptr;
        owner_check_ = owner_check_seal_ = nullptr;
        owner_guard_retired_ = true;
    }
    [[nodiscard]] bool owner_retirement_idle() const
    {
        return status_ && owner_guard_retired_ && !owner_context_ && !owner_context_seal_ &&
            !owner_check_ && !owner_check_seal_ && !control_.pending && !sealed_.pending &&
            same_control(control_, sealed_);
    }
    [[nodiscard]] bool continuing_owner() const
    {
        if (owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_)) return false;
        return !owner_check_ || bool(owner_check_(owner_context_));
    }
    [[nodiscard]] bool continuing_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = anchor_.observe_binding();
            return actual && original_binding_ == original_binding_seal_ && actual.value() == original_binding_;
        }
        else return true;
    }
    [[nodiscard]] bool terminal_binding() const
    {
        if constexpr (requires { original_->observe_terminal_binding(); })
        {
            const auto actual = anchor_.observe_terminal_binding();
            return actual && original_binding_ == original_binding_seal_ && actual.value() == original_binding_;
        }
        else if constexpr (requires { original_->observe_binding(); }) return false;
        else return true;
    }
    [[nodiscard]] bool rejection_binding() const
    { return continuing_binding() || terminal_binding(); }
    [[nodiscard]] bool retained_exact(bool validate_live_boundary = true) const
    {
        if (!attempted_ || !original_ || original_ != original_seal_ || !anchor_.exact(original_) || control_.phase != Phase::preparing ||
            original_binding_ != original_binding_seal_ ||
            owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_) ||
            (validate_live_boundary && (!continuing_binding() || !continuing_owner())) ||
            !same_control(control_, sealed_) || !stable(initial_) ||
            !reclaim_history::same_session(initial_, initial_seal_) ||
            !reclaim_history::same_session(progress_, progress_seal_) ||
            !reclaim_history::same_info(binding_, binding_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            control_.attempts != control_.completions || !stable(progress_)) return false;
        const auto bound = capacity(initial_, control_.groups);
        if (!bound || bound.value().reads != reserved_.reads || bound.value().groups != reserved_.groups ||
            binding_.metadata_count != control_.groups * 4 || control_.attempts > reserved_.reads)
            return false;
        auto expected = initial_;
        expected.reads += control_.attempts; expected.completed_reads += control_.completions;
        return reclaim_history::same_session(expected, progress_);
    }
    [[nodiscard]] Status idle_exact()
    {
        if (!retained_exact() || control_.pending)
            return reject(Status::fault("allocated empty data preview original idle seals changed"));
        const auto actual = original_->report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), progress_))
            return reject(Status::fault("allocated empty data preview original idle Session changed"));
        return {};
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained_exact() || !anchor_.exact(&session) || original_ != &session || !control_.pending ||
            !reclaim_history::same_session(flight_, flight_seal_) ||
            !reclaim_history::same_session(flight_, progress_))
            return Status::fault("allocated empty data preview original dispatch seals changed");
        const auto actual = session.report();
        if (!actual) return actual.status();
        observed_ = actual.value();
        auto expected = flight_; ++expected.reads; expected.in_flight = true;
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return Status::fault("allocated empty data preview original in-flight Session changed");
        return {};
    }
    [[nodiscard]] Status read(u32 number, std::span<u8> out)
    {
        if (auto status = idle_exact(); !status) return status;
        if (number >= binding_.layout.filesystem_blocks || out.size() != 1024 ||
            control_.attempts >= reserved_.reads)
            return reject(Status::overflow("allocated empty data preview selected READ bounds or credit"));
        flight_ = flight_seal_ = progress_;
        control_.sector = binding_.layout.first_sector + u64(number) * 2;
        control_.pending = true; sync();
        const auto transport = original_->read_blocks_guarded(control_.sector,
            std::as_writable_bytes(out), dispatch_);
        return finish_read(transport);
    }
    [[nodiscard]] Status finish_read(Status transport)
    {
        // The Session drains and quarantines a refused guarded dispatch before
        // exact failed-prefix accounting. Its live owner is already revoked.
        if (!transport && !terminal_binding())
            return refuse(Status::fault("allocated empty data preview original terminal binding changed"));
        if (!retained_exact(bool(transport)) || !control_.pending ||
            !reclaim_history::same_session(flight_, flight_seal_))
            return reject(Status::fault("allocated empty data preview post-READ seals changed"));
        const auto actual = original_->report();
        if (!actual) return reject(actual.status());
        observed_ = actual.value();
        auto expected = flight_; ++expected.reads;
        if (transport) ++expected.completed_reads;
        else
        {
            expected.state = block::DeviceState::quarantined; expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok)
                return reject(Status::fault("allocated empty data preview failed READ lacks quarantine"));
        }
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("allocated empty data preview READ attempt/completion prefix changed"));
        ++control_.attempts; if (transport) ++control_.completions;
        progress_ = progress_seal_ = actual.value();
        control_.pending = false; control_.sector = 0; sync();
        return transport ? Status{} : reject(transport);
    }
    [[nodiscard]] Status refuse(Status status)
    { control_.phase = Phase::failed; status_ = status; sync(); return status; }
    [[nodiscard]] Status reject(Status status)
    {
        if (!rejection_binding())
            return refuse(Status::fault("allocated empty data preview original rejection binding changed"));
        // The agreed original is captured once for rejection and final
        // observation; a single anchor drift must not leave an in-flight
        // report after the original request has already been drained.
        if (const auto actual = anchor_.abort_observe(status); actual) observed_ = actual.value();
        return refuse(status);
    }
    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{}, *original_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Ext4RecoveryInfo binding_{}, binding_seal_{};
    Ext4AllocatedEmptyDataReadCapacity reserved_{};
    Control control_{}, sealed_{};
    Status status_{};
    bool attempted_{};
    block::SessionBindingObservation original_binding_{}, original_binding_seal_{};
    void *owner_context_{}, *owner_context_seal_{};
    Status (*owner_check_)(void *){}, (*owner_check_seal_)(void *){};
    bool owner_guard_retired_{};
    Reader reader_{*this};
    Dispatch dispatch_{*this};
    Ext4AllocatedEmptyDataPreviewWorkspace<Reader> workspace_{};
    Ext4AllocatedEmptyDataPreview<Reader> preview_{};
};
} // namespace ok::fs
