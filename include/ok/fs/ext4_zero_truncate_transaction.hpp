#pragma once

#include "ok/fs/ext4_zero_truncate_journal.hpp"
#include "ok/fs/ext4_zero_truncate_session_preview.hpp"
#include "ok/fs/ext4_zero_truncate_transaction_budget.hpp"
#include <algorithm>

namespace ok::fs
{
enum class Ext4ZeroTruncateTransactionPhase : u8
{ unused, preparing, prepared, publishing, published, inode, allocations, cleanup, final, complete, failed };
struct Ext4ZeroTruncateTransactionReport
{
    Ext4ZeroTruncateTransactionPhase phase{};
    Status status{};
    Ext4ZeroTruncateTransactionCapacity reserved{};
    block::SessionReport initial{}, observed{};
    block::SessionCounter reads{}, writes{}, flushes{}, completed_reads{}, completed_writes{}, completed_flushes{};
    u32 inode{}, generation{}, released_count{}, final_sequence{};
    u8 home_after_mask{};
    bool noop{}, dispatch_pending{};
};

// A private original-Session transaction, not a mounted filesystem entry point.
// The canonical caller excludes all other Session users/raw writers and retains
// the service, device, Session and this off-stack storage through the attempt.
template <typename Session>
class Ext4ZeroTruncateTransaction final
{
    template <usize, usize> friend class Ext4OwnedTransaction;
    using Phase = Ext4ZeroTruncateTransactionPhase;
    using Route = Ext4ZeroTruncateRoute;
    using Block = Ext4ZeroTruncateBlock;
    enum class Operation : u8 { none, read, write, flush };
    struct Reader
    {
        Ext4ZeroTruncateTransaction &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        { return owner.read(number, bytes); }
    };
    struct GeneratedReader
    {
        Ext4ZeroTruncateTransaction &owner;
        [[nodiscard]] Status read_block(u32 number, std::span<u8> bytes)
        {
            if (auto status = owner.idle(); !status) return status;
            if (bytes.size() != 1024) return owner.reject(Status::invalid_argument("zero truncate generated read size"));
            const u64 first = owner.binding_.layout.journal_first_block;
            if (number >= first && u64(number) - first < 8)
            {
                const auto &image = owner.formatted(u32(u64(number) - first));
                for (usize at = 0; at < 1024; ++at) bytes[at] = image[at];
                return {};
            }
            return owner.read(number, bytes);
        }
    };
    struct Dispatch
    {
        Ext4ZeroTruncateTransaction &owner;
        [[nodiscard]] Status in_flight_exact(Session &session)
        { return owner.in_flight_exact(session); }
    };
    struct Images
    {
        Ext4ZeroTruncatePlan plan{};
        Ext4ZeroTruncateJournalFormat journal{};
        std::array<Block, 8> original_journal{};
        std::array<Block, 4> actual_homes{};
        Block inode_bitmap{};
        u32 inode_bitmap_target{};
        u8 initial_home_mask{};
        bool ready{};
    };
    struct Control
    {
        Phase phase{};
        Operation operation{};
        block::SessionCounter reads{}, writes{}, flushes{}, completed_reads{}, completed_writes{}, completed_flushes{};
        u32 journal_cursor{}, selected{};
        block::SessionCounter publication_flushes{}, checkpoint_flushes{};
        u64 sector{};
        u8 journal_mask{}, home_mask{};
        bool pending{}, pending_writes{}, cleanup{}, generated{}, final_view{};
        friend bool operator==(const Control &, const Control &) = default;
    };
  public:
    Ext4ZeroTruncateTransaction() = default;
    ~Ext4ZeroTruncateTransaction()
    {
        if (minted_ && control_.phase != Phase::complete)
            static_cast<void>(anchor_.abort(Status::interrupted("zero truncate private transaction unfinished")));
    }
    Ext4ZeroTruncateTransaction(const Ext4ZeroTruncateTransaction &) = delete;
    Ext4ZeroTruncateTransaction &operator=(const Ext4ZeroTruncateTransaction &) = delete;
    Ext4ZeroTruncateTransaction(Ext4ZeroTruncateTransaction &&) = delete;
    Ext4ZeroTruncateTransaction &operator=(Ext4ZeroTruncateTransaction &&) = delete;
    [[nodiscard]] Ext4ZeroTruncateTransactionReport report() const
    {
        return {control_.phase, status_, reserved_, initial_, observed_, control_.reads, control_.writes,
            control_.flushes, control_.completed_reads, control_.completed_writes, control_.completed_flushes,
            images_.plan.inode, images_.plan.generation, images_.plan.released_count, reserved_.final_sequence,
            control_.home_mask, route_ == Route::noop, control_.pending};
    }
  private:
    static bool overlap(const void *a, usize as, const void *b, usize bs)
    {
        const auto one = reinterpret_cast<usize>(a), two = reinterpret_cast<usize>(b);
        return one <= two ? two - one < as : one - two < bs;
    }
    static bool same_geometry(driver::BlockGeometry a, driver::BlockGeometry b)
    { return a.block_size == b.block_size && a.block_count == b.block_count && a.writable == b.writable; }
    static bool same_bounds(block::SessionBounds a, block::SessionBounds b)
    { return a.first == b.first && a.count == b.count; }
    static bool same_update(const Ext4ZeroTruncateUpdate &a, const Ext4ZeroTruncateUpdate &b)
    { return a.inode == b.inode && a.generation == b.generation && a.old_size == b.old_size && a.seconds == b.seconds && a.nanoseconds == b.nanoseconds; }
    static bool same_plan(const Ext4ZeroTruncatePlan &a, const Ext4ZeroTruncatePlan &b)
    {
        return a.inode == b.inode && a.generation == b.generation && a.old_size == b.old_size && a.group == b.group &&
            a.released_count == b.released_count && a.released_mapping == b.released_mapping && a.targets == b.targets &&
            a.before == b.before && a.after == b.after && a.count == b.count && a.noop == b.noop;
    }
    static bool same_journal(const Ext4ZeroTruncateJournalFormat &a, const Ext4ZeroTruncateJournalFormat &b)
    {
        return a.head == b.head && a.descriptor == b.descriptor && a.commit == b.commit && a.end == b.end &&
            a.empty_head == b.empty_head && a.payloads == b.payloads && a.active_sequence == b.active_sequence &&
            a.next_sequence == b.next_sequence && a.ready == b.ready;
    }
    static bool same_images(const Images &a, const Images &b)
    {
        return same_plan(a.plan, b.plan) && same_journal(a.journal, b.journal) &&
            a.original_journal == b.original_journal && a.actual_homes == b.actual_homes &&
            a.inode_bitmap == b.inode_bitmap && a.inode_bitmap_target == b.inode_bitmap_target &&
            a.initial_home_mask == b.initial_home_mask && a.ready == b.ready;
    }
    static u32 population(u8 mask)
    { u32 count{}; for (u32 bit = 0; bit < 4; ++bit) if (mask & (1U << bit)) ++count; return count; }
    void sync() { sealed_ = control_; }
    void seal_images() { images_.ready = true; images_seal_ = images_; }
    [[nodiscard]] Status refuse(Status status)
    { control_.phase = Phase::failed; status_ = status; sync(); return status; }
    [[nodiscard]] Status reject(Status status)
    {
        if (minted_ && !rejection_binding())
            return refuse(Status::fault("zero truncate original rejection binding changed"));
        if (const auto actual = anchor_.abort_observe(status); actual) observed_ = actual.value();
        return refuse(status);
    }
    [[nodiscard]] Status mint(Session &session, const Ext4RecoveryInfo &binding,
                              Route route, Ext4ZeroTruncateUpdate update = {})
    {
        if (attempted_) return Status::already_exists("zero truncate private transaction is one-shot");
        if (overlap(this, sizeof(*this), &session, sizeof(session)) || overlap(this, sizeof(*this), &binding, sizeof(binding)))
            return Status::invalid_argument("zero truncate private transaction input overlap");
        attempted_ = true;
        if (auto status = anchor_.bind(session); !status) return refuse(status);
        original_ = original_seal_ = &session;
        if constexpr (requires { session.observe_binding(); })
        {
            const auto actual_binding = session.observe_binding();
            if (!actual_binding) return refuse(actual_binding.status());
            authoritative_binding_ = authoritative_binding_seal_ = actual_binding.value();
        }
        const auto actual = session.report(); if (!actual) return refuse(actual.status());
        observed_ = actual.value();
        if (!binding.metadata_count || binding.metadata_count % 4 || binding.metadata_count > binding.metadata.size() ||
            !binding.structural_metadata().valid() || !binding.needs_recovery ||
            binding.empty_log != (route != Route::recovery))
            return refuse(Status::invalid_argument("zero truncate private transaction binding profile"));
        const auto capacity = Ext4ZeroTruncateTransactionBudget::capacity(actual.value(), u32(binding.metadata_count / 4), route, binding.journal_sequence);
        if (!capacity) return refuse(capacity.status());
        const auto geometry = session.geometry(); const auto bounds = session.bounds();
        const auto limit = session.transfer_limit(); if (!limit) return refuse(limit.status());
        if (geometry.block_size != 512 || !geometry.block_count || bounds.count != geometry.block_count ||
            bounds.first > ~u64{0} - bounds.count || limit.value() < 1024 ||
            (route != Route::noop && !geometry.writable) || binding.layout.block_size != 1024 ||
            !binding.layout.filesystem_blocks || binding.layout.filesystem_blocks > 0xffffffffULL ||
            binding.layout.first_sector >= geometry.block_count ||
            binding.layout.filesystem_blocks > (geometry.block_count - binding.layout.first_sector) / 2)
            return refuse(Status::unsupported("zero truncate private transaction volume geometry"));
        binding_ = binding_seal_ = binding; request_ = request_seal_ = update;
        route_ = route_seal_ = route; reserved_ = capacity.value();
        initial_ = initial_seal_ = progress_ = progress_seal_ = actual.value();
        geometry_ = geometry_seal_ = geometry; bounds_ = bounds_seal_ = bounds;
        control_.phase = Phase::preparing; sync(); minted_ = true;
        return idle();
    }
    // Real BlockSessions can observe continuing registered backing without
    // callbacks or transport. Generic model proxies retain their old contract.
    [[nodiscard]] bool continuing_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = original_->observe_binding();
            return actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_;
        }
        else return true;
    }
    [[nodiscard]] bool terminal_binding() const
    {
        if constexpr (requires { original_->observe_terminal_binding(); })
        {
            const auto actual = anchor_.observe_terminal_binding();
            return actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_;
        }
        else if constexpr (requires { original_->observe_binding(); }) return false;
        else return true;
    }
    [[nodiscard]] bool rejection_binding() const
    {
        if constexpr (requires { original_->observe_binding(); })
        {
            const auto actual = anchor_.observe_binding();
            if (actual && authoritative_binding_ == authoritative_binding_seal_ &&
                actual.value() == authoritative_binding_) return true;
            return terminal_binding();
        }
        else return true;
    }
    [[nodiscard]] Status bind_owner_guard(void *context, Status (*check)(void *))
    {
        if (attempted_ || owner_guard_retired_ || owner_context_ || owner_check_ || !context || !check)
            return Status::already_exists("typed child owner attachment is one-shot");
        owner_context_ = owner_context_seal_ = context;
        owner_check_ = owner_check_seal_ = check;
        return {};
    }
    // The actual owner calls this on every terminal exit before its local
    // context dies. Retired callbacks cannot be rebound or dereferenced later.
    void retire_owner_guard()
    {
        recovery_preview_.retire_owner_guard();
        owner_context_ = owner_context_seal_ = nullptr;
        owner_check_ = owner_check_seal_ = nullptr;
        owner_guard_retired_ = true;
    }
    [[nodiscard]] Status retire_aborted_owner_session(Session &session)
    {
        if (!owner_guard_retired_ || owner_context_ || owner_context_seal_ || owner_check_ || owner_check_seal_ ||
            !recovery_preview_.owner_guard_retired_ || recovery_preview_.owner_context_ ||
            recovery_preview_.owner_context_seal_ || recovery_preview_.owner_check_ || recovery_preview_.owner_check_seal_)
            return Status::busy("zero failed Session retirement requires retired Owner callbacks");
        if (recovery_preview_.anchor_.exact(&session))
            if (auto status = recovery_preview_.anchor_.retire_aborted_original(session); !status) return status;
        if (anchor_.exact(&session)) return anchor_.retire_aborted_original(session);
        return {};
    }
    [[nodiscard]] bool owner_retirement_idle() const
    {
        return status_ && owner_guard_retired_ && !owner_context_ && !owner_context_seal_ &&
            !owner_check_ && !owner_check_seal_ && !control_.pending && !sealed_.pending &&
            !control_.pending_writes && !sealed_.pending_writes && control_ == sealed_ &&
            (minted_ ? control_.phase == Phase::complete :
                (!attempted_ && !original_ && !original_seal_ && control_ == Control{})) &&
            recovery_preview_.owner_retirement_idle();
    }
    [[nodiscard]] bool continuing_owner() const
    {
        if (owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_)) return false;
        return !owner_check_ || bool(owner_check_(owner_context_));
    }
    [[nodiscard]] bool completed_owner_callbacks_retired() const
    {
        const auto &preview = recovery_preview_;
        return owner_guard_retired_ && !owner_context_ && !owner_context_seal_ && !owner_check_ && !owner_check_seal_ &&
            preview.owner_guard_retired_ && !preview.owner_context_ && !preview.owner_context_seal_ &&
            !preview.owner_check_ && !preview.owner_check_seal_;
    }
    [[nodiscard]] bool retained(bool validate_live_boundary = true) const
    {
        if (!minted_ || !original_ || original_ != original_seal_ || !anchor_.exact(original_) ||
            control_.phase == Phase::failed || control_.phase == Phase::unused || control_ != sealed_ ||
            authoritative_binding_ != authoritative_binding_seal_ ||
            owner_context_ != owner_context_seal_ || owner_check_ != owner_check_seal_ ||
            bool(owner_context_) != bool(owner_check_) ||
            !completed_ownership_birth_valid() ||
            (validate_live_boundary && (!continuing_binding() || !continuing_owner())) ||
            route_ != route_seal_ || !same_update(request_, request_seal_) ||
            !reclaim_history::same_info(binding_, binding_seal_) ||
            !reclaim_history::same_session(initial_, initial_seal_) || !reclaim_history::same_session(progress_, progress_seal_) ||
            !same_geometry(geometry_, geometry_seal_) || !same_bounds(bounds_, bounds_seal_) ||
            (images_.ready && !same_images(images_, images_seal_)) || images_.ready != images_seal_.ready ||
            (control_.generated && !reclaim_history::same_info(generated_binding_, generated_binding_seal_)) ||
            (control_.final_view && !reclaim_history::same_info(final_binding_, final_binding_seal_))) return false;
        const auto capacity = Ext4ZeroTruncateTransactionBudget::capacity(initial_, u32(binding_.metadata_count / 4), route_, binding_.journal_sequence);
        if (!capacity || capacity.value() != reserved_ || control_.reads > reserved_.reads || control_.writes > reserved_.writes ||
            control_.flushes > reserved_.flushes || control_.reads != control_.completed_reads ||
            control_.writes != control_.completed_writes || control_.flushes != control_.completed_flushes ||
            control_.journal_cursor > 8 || control_.publication_flushes > 3 || control_.checkpoint_flushes > 3 ||
            (control_.home_mask & ~u8{15}) || (control_.home_mask & images_.initial_home_mask) != images_.initial_home_mask)
            return false;
        constexpr std::array<u32, 8> order{1, 2, 3, 4, 5, 7, 6, 0};
        u8 journal_mask = route_ == Route::recovery && images_.ready ? 255 : 0;
        if (route_ == Route::fresh)
            for (u32 at = 0; at < control_.journal_cursor; ++at) journal_mask |= u8(1U << order[at]);
        else if (control_.journal_cursor || control_.publication_flushes) return false;
        if (control_.journal_mask != journal_mask ||
            control_.writes != (route_ == Route::fresh ? control_.journal_cursor : 0) +
                population(u8(control_.home_mask & ~images_.initial_home_mask)) + (control_.cleanup ? 1 : 0) ||
            control_.flushes != control_.publication_flushes + control_.checkpoint_flushes ||
            (control_.checkpoint_flushes && !(control_.home_mask & 1)) ||
            (control_.checkpoint_flushes >= 2 && control_.home_mask != 15) ||
            (control_.cleanup && control_.checkpoint_flushes < 2)) return false;
        auto expected = initial_;
        expected.reads += control_.reads; expected.completed_reads += control_.completed_reads;
        expected.writes += control_.writes; expected.completed_writes += control_.completed_writes;
        expected.flushes += control_.flushes; expected.completed_flushes += control_.completed_flushes;
        expected.pending_writes = control_.pending_writes;
        return reclaim_history::same_session(expected, progress_);
    }
    [[nodiscard]] bool completed_ownership_birth_valid(bool require_complete = false) const
    {
        using Validator = Ext4ZeroTruncateOwnershipValidator;
        const bool complete = require_complete || control_.phase == Phase::complete;
        if (!Validator::completed_birth_progress_exact(ownership_birth_, ownership_birth_progress_, this)) return false;
        if (Validator::completed_birth_uncaptured(ownership_birth_, ownership_birth_progress_, this)) return !complete;
        if (&reader_.owner != this || (complete && !Validator::completed_birth_complete(ownership_birth_))) return false;
        return route_ == Route::noop ?
            Validator::completed_birth_exact(old_view_, reader_, binding_, old_proof_, ownership_birth_, this, complete) :
            Validator::completed_birth_exact(final_view_, reader_, final_binding_, final_proof_, ownership_birth_, this, complete);
    }
    [[nodiscard]] Status idle()
    {
        if (!retained() || control_.pending) return reject(Status::fault("zero truncate private original idle seals changed"));
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), progress_))
            return reject(Status::fault("zero truncate private original idle Session changed"));
        return {};
    }
    [[nodiscard]] Status in_flight_exact(Session &session)
    {
        if (!retained() || !control_.pending || control_.operation == Operation::none ||
            !anchor_.exact(&session) || original_ != &session || !reclaim_history::same_session(flight_, flight_seal_) ||
            !reclaim_history::same_session(flight_, progress_))
            return reject(Status::fault("zero truncate private dispatch seals changed"));
        auto expected = flight_; expected.in_flight = true;
        if (control_.operation == Operation::read) ++expected.reads;
        if (control_.operation == Operation::write) { ++expected.writes; expected.pending_writes = true; }
        if (control_.operation == Operation::flush) ++expected.flushes;
        const auto actual = session.report(); if (!actual) return reject(actual.status());
        observed_ = actual.value();
        if (!same_geometry(session.geometry(), geometry_) || !same_bounds(session.bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("zero truncate private in-flight Session changed"));
        return {};
    }
    [[nodiscard]] Status finish_io(Status transport)
    {
        // Prove the opening backing before any post-failure report or abort.
        // A same-number foreign quarantine must never supply completion credit.
        if (!transport && !terminal_binding())
            return refuse(Status::fault("zero truncate original terminal binding changed"));
        // Failed guarded transport has already quarantined the Session and
        // removed its live owner. Retain every fixed seal, account the exact
        // failed prefix and stop; no further live admission or I/O is attempted.
        if (!retained(bool(transport)) || !control_.pending || !reclaim_history::same_session(flight_, flight_seal_))
            return reject(Status::fault("zero truncate private post-return seals changed"));
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        observed_ = actual.value(); auto expected = flight_;
        if (control_.operation == Operation::read) { ++expected.reads; if (transport) ++expected.completed_reads; }
        if (control_.operation == Operation::write)
        { ++expected.writes; expected.pending_writes = true; if (transport) ++expected.completed_writes; }
        if (control_.operation == Operation::flush)
        { ++expected.flushes; if (transport) { ++expected.completed_flushes; expected.pending_writes = false; } }
        if (!transport)
        {
            expected.state = block::DeviceState::quarantined; expected.first_error = actual.value().first_error;
            if (expected.first_error == StatusCode::ok) return reject(Status::fault("zero truncate failed I/O lacks quarantine"));
        }
        if (!same_geometry(original_->geometry(), geometry_) || !same_bounds(original_->bounds(), bounds_) ||
            !reclaim_history::same_session(actual.value(), expected))
            return reject(Status::fault("zero truncate exact attempted/completed I/O prefix changed"));
        if (!transport)
        {
            if (control_.operation == Operation::read) ++control_.reads;
            if (control_.operation == Operation::write) ++control_.writes;
            if (control_.operation == Operation::flush) ++control_.flushes;
            progress_ = progress_seal_ = actual.value(); control_.pending_writes = actual.value().pending_writes;
            control_.pending = false; control_.operation = Operation::none; control_.sector = 0; control_.selected = 0;
            return reject(transport);
        }
        if (control_.operation == Operation::read) { ++control_.reads; ++control_.completed_reads; }
        if (control_.operation == Operation::write)
        {
            ++control_.writes; ++control_.completed_writes;
            if (control_.phase == Phase::publishing)
            { control_.journal_mask |= u8(1U << control_.selected); ++control_.journal_cursor; }
            else if (control_.phase == Phase::cleanup) control_.cleanup = true;
            else control_.home_mask |= u8(1U << control_.selected);
        }
        if (control_.operation == Operation::flush)
        {
            ++control_.flushes; ++control_.completed_flushes;
            if (control_.phase == Phase::publishing) ++control_.publication_flushes;
            else ++control_.checkpoint_flushes;
        }
        progress_ = progress_seal_ = actual.value(); control_.pending_writes = actual.value().pending_writes;
        control_.pending = false; control_.operation = Operation::none; control_.sector = 0; control_.selected = 0; sync();
        return idle();
    }
    [[nodiscard]] Status begin_io(Operation kind, u32 selected, u32 physical = 0)
    {
        if (auto status = idle(); !status) return status;
        if ((kind == Operation::read && (control_.reads == reserved_.reads || control_.pending_writes)) ||
            (kind == Operation::write && control_.writes == reserved_.writes) ||
            (kind == Operation::flush && control_.flushes == reserved_.flushes) ||
            (kind != Operation::flush && physical >= binding_.layout.filesystem_blocks))
            return reject(Status::overflow("zero truncate selected I/O bound or whole credit"));
        flight_ = flight_seal_ = progress_; control_.operation = kind; control_.selected = selected;
        control_.sector = kind == Operation::flush ? 0 : binding_.layout.first_sector + u64(physical) * 2;
        control_.pending = true; sync(); return {};
    }
    [[nodiscard]] Status read(u32 number, std::span<u8> bytes)
    {
        if (bytes.size() != 1024) return reject(Status::invalid_argument("zero truncate selected READ size"));
        if (auto status = begin_io(Operation::read, number, number); !status) return status;
        return finish_io(original_->read_blocks_guarded(control_.sector, std::as_writable_bytes(bytes), dispatch_));
    }
    const Block &formatted(u32 record) const
    {
        if (!record) return images_.journal.head;
        if (record == 1) return images_.journal.descriptor;
        if (record < 6) return images_.journal.payloads[record - 2];
        return record == 6 ? images_.journal.commit : images_.journal.end;
    }
    [[nodiscard]] Status write_selected()
    {
        if (auto status = idle(); !status) return status;
        u32 selected{}, physical{}; const Block *image{};
        if (control_.phase == Phase::publishing && route_ == Route::fresh)
        {
            constexpr std::array<u32, 8> order{1, 2, 3, 4, 5, 7, 6, 0};
            if (control_.journal_cursor >= 8 || control_.publication_flushes !=
                (control_.journal_cursor < 6 ? 0U : control_.journal_cursor - 5))
                return reject(Status::denied("zero truncate WAL write is not the selected stage"));
            selected = order[control_.journal_cursor]; physical = u32(binding_.layout.journal_first_block) + selected;
            image = &formatted(selected);
        }
        else if (control_.phase == Phase::inode && !control_.checkpoint_flushes && !(control_.home_mask & 1))
        { selected = 0; physical = images_.plan.targets[0]; image = &images_.plan.after[0]; }
        else if (control_.phase == Phase::allocations && control_.checkpoint_flushes == 1 && (control_.home_mask & 1))
        {
            selected = 1; while (selected < 4 && (control_.home_mask & (1U << selected))) ++selected;
            if (selected == 4) return reject(Status::denied("zero truncate allocation stage is complete"));
            physical = images_.plan.targets[selected]; image = &images_.plan.after[selected];
        }
        else if (control_.phase == Phase::cleanup && control_.checkpoint_flushes == 2 &&
                 control_.home_mask == 15 && !control_.cleanup)
        { physical = u32(binding_.layout.journal_first_block); image = &images_.journal.empty_head; }
        else return reject(Status::denied("zero truncate WRITE lacks private selected stage"));
        if (!images_.ready || images_.plan.count != 4 || !images_.journal.ready)
            return reject(Status::fault("zero truncate WRITE lacks complete retained images"));
        if (auto status = begin_io(Operation::write, selected, physical); !status) return status;
        return finish_io(original_->write_blocks_guarded(control_.sector, std::as_bytes(std::span{*image}), dispatch_));
    }
    [[nodiscard]] Status flush_selected()
    {
        if (auto status = idle(); !status) return status;
        const bool publication = control_.phase == Phase::publishing && route_ == Route::fresh &&
            ((control_.journal_cursor == 6 && !control_.publication_flushes) ||
             (control_.journal_cursor == 7 && control_.publication_flushes == 1) ||
             (control_.journal_cursor == 8 && control_.publication_flushes == 2));
        const bool checkpoint = (control_.phase == Phase::inode && !control_.checkpoint_flushes && (control_.home_mask & 1)) ||
            (control_.phase == Phase::allocations && control_.checkpoint_flushes == 1 && control_.home_mask == 15) ||
            (control_.phase == Phase::cleanup && control_.checkpoint_flushes == 2 && control_.cleanup);
        if (!publication && !checkpoint) return reject(Status::denied("zero truncate FLUSH lacks private selected barrier"));
        if (auto status = begin_io(Operation::flush, 0); !status) return status;
        return finish_io(original_->flush_guarded(dispatch_));
    }
    [[nodiscard]] Status physical()
    {
        if (auto status = idle(); !status) return status;
        if (!images_.ready || control_.pending_writes) return reject(Status::fault("zero truncate physical readback lacks stable history"));
        const auto check = [&](u32 number, const Block &expected) -> Status {
            if (auto status = read(number, checking_); !status) return status;
            return checking_ == expected ? Status{} : reject(Status::io_error("zero truncate exact physical prefix changed"));
        };
        for (u32 record = 0; record < 8; ++record)
        {
            const auto &expected = !record && control_.cleanup ? images_.journal.empty_head :
                control_.journal_mask & (1U << record) ? formatted(record) : images_.original_journal[record];
            if (auto status = check(u32(binding_.layout.journal_first_block) + record, expected); !status) return status;
        }
        if (auto status = check(images_.inode_bitmap_target, images_.inode_bitmap); !status) return status;
        for (u32 home = 0; home < 4; ++home)
            if (auto status = check(images_.plan.targets[home], control_.home_mask & (1U << home) ?
                    images_.plan.after[home] : images_.plan.before[home]); !status) return status;
        return idle();
    }
    [[nodiscard]] Status prepare(Session &session, const Ext4RecoveryInfo &binding, Ext4ZeroTruncateUpdate update)
    {
        if (!update.inode || update.old_size > 16384 ||
            (update.old_size && (update.seconds > 0x7fffffffU || update.nanoseconds >= 1000000000U)))
            return Status::invalid_argument("zero truncate private update profile");
        if (auto status = mint(session, binding, update.old_size ? Route::fresh : Route::noop, update); !status) return status;
        if (auto status = old_view_.open(reader_, binding_, opening_); !status) return reject(status);
        const auto ownership = route_ == Route::noop ?
            Ext4ZeroTruncateOwnershipValidator::prove_completed(binding_, old_view_, reader_, request_, ownership_, old_proof_, ownership_birth_, ownership_birth_progress_, this) :
            Ext4ZeroTruncateOwnershipValidator::prove(binding_, old_view_, request_, ownership_, old_proof_);
        if (!ownership)
            return reject(ownership);
        const u32 group = old_proof_.count ? old_proof_.group : 0;
        const u32 per_group = ext4_checked::le32(ownership_.superblock, 40);
        const u32 inode_group = (request_.inode - 1) / per_group, inode_local = (request_.inode - 1) % per_group;
        const u32 table = ext4_checked::le32(ownership_.descriptors, inode_group * 32 + 8) + inode_local / 4;
        const u32 bitmap = ext4_checked::le32(ownership_.descriptors, group * 32);
        images_.inode_bitmap_target = ext4_checked::le32(ownership_.descriptors, inode_group * 32 + 4);
        const std::array<u32, 4> homes{table, bitmap, 2, 1};
        for (u32 record = 0; record < 8; ++record)
        {
            if (auto status = read(u32(binding_.layout.journal_first_block) + record, checking_); !status) return status;
            images_.original_journal[record] = checking_;
        }
        for (u32 home = 0; home < 4; ++home)
        {
            if (auto status = read(homes[home], checking_); !status) return status;
            images_.plan.before[home] = checking_;
        }
        if (auto status = read(images_.inode_bitmap_target, checking_); !status) return status;
        images_.inode_bitmap = checking_;
        if (images_.plan.before[3] != ownership_.superblock || images_.plan.before[2] != ownership_.descriptors ||
            !std::equal(ownership_.inode.begin(), ownership_.inode.end(), images_.plan.before[0].begin() + (inode_local % 4) * 256))
            return reject(Status::fault("zero truncate captured old identity differs from complete proof"));
        if (route_ == Route::noop)
        {
            const auto head = jbd2::Codec::open(images_.original_journal[0], u32(binding_.layout.journal_blocks), binding_.uuid,
                jbd2::DescriptorUuidPolicy::linux_internal_zero);
            if (!head) return reject(head.status());
            if (head.value().superblock().start_block || head.value().superblock().sequence != binding_.journal_sequence ||
                head.value().superblock().block_size != 1024 || head.value().superblock().first_block != 1)
                return reject(Status::unsupported("zero truncate no-op dirty empty journal differs"));
            images_.plan.inode = request_.inode; images_.plan.generation = request_.generation;
            images_.plan.group = group; images_.plan.targets = homes; images_.plan.after = images_.plan.before;
            images_.plan.noop = true; seal_images();
            if (auto status = physical(); !status) return status;
            control_.phase = Phase::complete; sync(); return idle();
        }
        const Ext4ZeroTruncateSource source{group, table, images_.plan.before[3], images_.plan.before[2],
            images_.plan.before[1], images_.inode_bitmap, images_.plan.before[0]};
        if (auto status = Ext4ZeroTruncatePlanner::plan_off_stack(binding_, request_, source, planning_, planned_); !status)
            return reject(status);
        if (planned_.released_mapping != old_proof_.mapping || planned_.released_count != old_proof_.count ||
            planned_.group != old_proof_.group || planned_.targets != homes)
            return reject(Status::fault("zero truncate plan differs from original complete ownership"));
        images_.plan = planned_;
        if (auto status = Ext4ZeroTruncateJournalFormatter::format(binding_, images_.plan, images_.original_journal[0],
                images_.inode_bitmap, formatter_, images_.journal); !status) return reject(status);
        seal_images(); generated_binding_ = generated_binding_seal_ = binding_;
        generated_binding_.empty_log = generated_binding_seal_.empty_log = false;
        generated_binding_.journal_sequence = generated_binding_seal_.journal_sequence = images_.journal.active_sequence;
        control_.generated = true; sync();
        if (auto status = generated_preview_.prepare(generated_reader_, generated_binding_, generated_workspace_); !status)
            return reject(status);
        if (generated_workspace_.claimant_ != &generated_preview_ || generated_preview_.workspace_ != &generated_workspace_ ||
            !same_plan(generated_workspace_.journal_.decoded, images_.plan) || generated_workspace_.result_.after_mask ||
            generated_preview_.report_.phase != Ext4ZeroTruncatePreviewPhase::ready)
            return reject(Status::fault("zero truncate generated complete preview differs"));
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::prepared; sync(); return idle();
    }
    [[nodiscard]] Status prepare_recovery(Session &session, const Ext4RecoveryInfo &binding)
    {
        if (auto status = mint(session, binding, Route::recovery); !status) return status;
        // The delegated reader retains the actual parent, whose fixed checks
        // include the outer canonical Owner/cache context on every dispatch.
        if (auto status = recovery_preview_.bind_owner_guard(this, recovery_owner_guard); !status)
        { recovery_preview_.retire_owner_guard(); return reject(status); }
        const auto preview_status = recovery_preview_.prepare(session, binding_);
        recovery_preview_.retire_owner_guard();
        if (!preview_status) return reject(preview_status);
        if (!retained()) return reject(Status::fault("zero truncate recovery parent changed during readonly preview"));
        const auto &child = recovery_preview_; const auto &workspace = child.workspace_;
        const auto actual = original_->report(); if (!actual) return reject(actual.status());
        const block::SessionCounter reads = child.control_.attempts;
        if (reads > reserved_.reads)
            return reject(Status::fault("zero truncate recovery preview exceeds whole reserved reads"));
        auto expected = progress_; expected.reads += reads; expected.completed_reads += reads;
        if (reads > reserved_.reads || child.control_.completions != reads || child.control_.phase != Ext4ZeroTruncateSessionPreviewPhase::ready ||
            !child.anchor_.exact(original_) || child.original_ != original_ || child.original_seal_ != original_ ||
            !reclaim_history::same_info(child.binding_, binding_) || !reclaim_history::same_info(child.binding_seal_, binding_) ||
            !reclaim_history::same_session(child.initial_, progress_) || !reclaim_history::same_session(child.initial_seal_, progress_) ||
            !reclaim_history::same_session(child.progress_, expected) || !reclaim_history::same_session(child.progress_seal_, expected) ||
            !reclaim_history::same_session(actual.value(), expected) || !reclaim_history::same_session(child.observed_, expected) ||
            !child.same_control(child.control_, child.sealed_) || child.control_.pending || !child.status_ ||
            child.original_binding_ != authoritative_binding_ || child.original_binding_seal_ != authoritative_binding_ ||
            !child.owner_guard_retired_ || child.owner_context_ || child.owner_context_seal_ ||
            child.owner_check_ || child.owner_check_seal_ ||
            !same_geometry(child.geometry_, geometry_) || !same_geometry(child.geometry_seal_, geometry_) ||
            !same_bounds(child.bounds_, bounds_) || !same_bounds(child.bounds_seal_, bounds_) ||
            workspace.claimant_ != &child.preview_ ||
            child.preview_.workspace_ != &workspace || workspace.reader_ != &child.reader_ || !workspace.journal_ready_ ||
            child.preview_.report_.phase != Ext4ZeroTruncatePreviewPhase::ready)
            return reject(Status::fault("zero truncate recovery exact private preview handoff differs"));
        control_.reads = control_.completed_reads = reads; progress_ = progress_seal_ = observed_ = actual.value(); sync();
        images_.plan = workspace.journal_.decoded; images_.inode_bitmap = workspace.inode_bitmap_;
        images_.inode_bitmap_target = workspace.inode_bitmap_target_; images_.actual_homes = workspace.homes_;
        images_.initial_home_mask = control_.home_mask = workspace.result_.after_mask;
        images_.original_journal[0] = workspace.head_; images_.original_journal[1] = workspace.descriptor_;
        for (u32 at = 0; at < 4; ++at) images_.original_journal[at + 2] = workspace.payloads_[at];
        images_.original_journal[6] = workspace.commit_; images_.original_journal[7] = workspace.end_;
        format_binding_ = binding_; format_binding_.empty_log = true; --format_binding_.journal_sequence;
        format_head_ = workspace.head_;
        be32(format_head_, 24, format_binding_.journal_sequence); be32(format_head_, 28, 0); stamp_head(format_head_);
        if (auto status = Ext4ZeroTruncateJournalFormatter::format(format_binding_, images_.plan, format_head_,
                images_.inode_bitmap, formatter_, images_.journal); !status) return reject(status);
        for (u32 record = 0; record < 8; ++record)
            if (formatted(record) != images_.original_journal[record])
                return reject(Status::fault("zero truncate recovery regenerated active WAL differs"));
        control_.journal_mask = 255; seal_images(); control_.phase = Phase::published; sync(); return idle();
    }
    [[nodiscard]] static Status recovery_owner_guard(void *raw)
    {
        auto &parent = *static_cast<Ext4ZeroTruncateTransaction *>(raw);
        return parent.retained() ? Status{} :
            Status::fault("zero truncate delegated original parent/Owner changed");
    }
    [[nodiscard]] Status publish()
    {
        if (control_.phase != Phase::prepared || route_ != Route::fresh)
            return Status::bad_descriptor("zero truncate publication lacks private fresh preparation");
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::publishing; sync();
        while (control_.journal_cursor < 6) if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::published; sync(); return idle();
    }
    [[nodiscard]] Status checkpoint()
    {
        if (control_.phase != Phase::published || route_ == Route::noop)
            return Status::bad_descriptor("zero truncate checkpoint lacks private complete publication");
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::inode; sync();
        if (!(control_.home_mask & 1)) if (auto status = write_selected(); !status) return status;
        // Obtain this barrier even when an inconsistent legacy mixture already
        // contains allocation-after homes and an inode-after image.
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::allocations; sync();
        while (control_.home_mask != 15) if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::cleanup; sync();
        if (auto status = write_selected(); !status) return status;
        if (auto status = flush_selected(); !status) return status;
        if (auto status = physical(); !status) return status;
        final_binding_ = final_binding_seal_ = binding_;
        final_binding_.empty_log = final_binding_seal_.empty_log = true;
        final_binding_.journal_sequence = final_binding_seal_.journal_sequence = reserved_.final_sequence;
        control_.phase = Phase::final; control_.final_view = true; sync();
        if (auto status = final_view_.open(reader_, final_binding_, opening_); !status) return reject(status);
        const Ext4ZeroTruncateUpdate empty{images_.plan.inode, images_.plan.generation, 0, 0, 0};
        if (auto status = Ext4ZeroTruncateOwnershipValidator::prove_completed(final_binding_, final_view_, reader_,
                empty, ownership_, final_proof_, ownership_birth_, ownership_birth_progress_, this); !status)
            return reject(status);
        if (final_proof_.count || final_proof_.inode != images_.plan.inode || final_proof_.generation != images_.plan.generation)
            return reject(Status::fault("zero truncate final physical ownership identity differs"));
        if (auto status = physical(); !status) return status;
        control_.phase = Phase::complete; sync(); return idle();
    }
    static void be32(Block &bytes, usize at, u32 value)
    { for (usize index = 0; index < 4; ++index) bytes[at + index] = u8(value >> (24 - index * 8)); }
    static void stamp_head(Block &bytes)
    { be32(bytes, 252, 0); be32(bytes, 252, jbd2::detail::crc32c(~u32{0}, bytes)); }

    reclaim_history::SessionAnchor<Session> anchor_{};
    Session *original_{}, *original_seal_{};
    Route route_{}, route_seal_{};
    Ext4RecoveryInfo binding_{}, binding_seal_{}, generated_binding_{}, generated_binding_seal_{},
        final_binding_{}, final_binding_seal_{}, format_binding_{};
    Ext4ZeroTruncateUpdate request_{}, request_seal_{};
    block::SessionReport initial_{}, initial_seal_{}, progress_{}, progress_seal_{}, flight_{}, flight_seal_{}, observed_{};
    bool owner_guard_retired_{};
    void *owner_context_{}, *owner_context_seal_{};
    Status (*owner_check_)(void *){}, (*owner_check_seal_)(void *){};
    block::SessionBindingObservation authoritative_binding_{}, authoritative_binding_seal_{};
    driver::BlockGeometry geometry_{}, geometry_seal_{};
    block::SessionBounds bounds_{}, bounds_seal_{};
    Ext4ZeroTruncateTransactionCapacity reserved_{};
    Control control_{}, sealed_{};
    Images images_{}, images_seal_{};
    Status status_{};
    bool attempted_{}, minted_{};
    Block checking_{}, format_head_{};
    Reader reader_{*this}; GeneratedReader generated_reader_{*this}; Dispatch dispatch_{*this};
    Ext4ReadOnlyOpenScratch opening_{};
    Ext4ReadOnlyView<Reader> old_view_{}, final_view_{};
    Ext4ZeroTruncateOwnershipScratch ownership_{};
    Ext4ZeroTruncateOwnership old_proof_{}, final_proof_{};
    Ext4ZeroTruncateOwnershipValidator::CompletedBirthSeal ownership_birth_{};
    Ext4ZeroTruncateOwnershipValidator::CompletedBirthProgress ownership_birth_progress_{};
    Ext4ZeroTruncatePlan planning_{}, planned_{};
    Ext4ZeroTruncateJournalFormatScratch formatter_{};
    Ext4ZeroTruncatePreviewWorkspace<GeneratedReader> generated_workspace_{};
    Ext4ZeroTruncatePreview<GeneratedReader> generated_preview_{};
    Ext4ZeroTruncateSessionPreview<Session> recovery_preview_{};
};
} // namespace ok::fs
