#pragma once

#include "ok/block/guarded_reads.hpp"
#include "ok/fs/ext4_direct_read_scope.hpp"
#include "ok/fs/ext4_reclaim_route_budget.hpp"
#include "ok/fs/ext4_orphan_chain_capture.hpp"
#include "ok/fs/metadata_buffers.hpp"
#include <new>

namespace ok::tests
{
namespace guarded_read_model
{
using Registry = block::BlockSessions<1>;
using Session = Registry::Session;
using Budget = fs::Ext4ReclaimRouteBudget<Session>;

class Device final : public driver::BlockDevice
{
  public:
    driver::BlockGeometry geometry() const override { return {sectors, 512, true}; }
    Status read_blocks(u64, std::span<std::byte> out) override
    {
        ++reads;
        if (observe) if (auto status = observe(context); !status) return status;
        if (!failure) return failure;
        for (auto &byte : out) byte = std::byte{0x5a};
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("read guard model cannot write"); }
    Status flush() override
    { ++flushes; return Status::denied("read guard model cannot flush"); }
    u64 sectors{64};
    usize reads{}, writes{}, flushes{};
    Status failure{};
    Status (*observe)(void *){};
    void *context{};
};

// The cache lends its original Session only during the synchronous hooks.
class Guard final
{
  public:
    explicit Guard(Device &device) : device_(device)
    {
        device_.context = this;
        device_.observe = [](void *context) -> Status {
            auto &self = *static_cast<Guard *>(context);
            return self.original_ ? self.budget_.in_flight_exact(*self.original_) :
                Status::fault("read guard model callback before binding");
        };
    }
    Guard(const Guard &) = delete;
    Guard &operator=(const Guard &) = delete;
    Guard(Guard &&) = delete;
    Guard &operator=(Guard &&) = delete;
    Status begin_read(Session &session, u64 first, usize bytes)
    {
        ++begins;
        if (refuse_begin) return Status::overflow("read guard model refused capture begin");
        if (!original_)
        {
            original_ = &session;
            if (auto status = budget_.reserve_opening(session); !status) return status;
            if (auto status = budget_.refine(session, 0, 42); !status) return status;
        }
        const auto status = budget_.begin_read(session, first, bytes);
        if (status && change_geometry) --device_.sectors;
        return status;
    }
    Status finish_read(Session &session, Status transport)
    {
        ++finishes;
        const auto status = budget_.finish_read(session, transport);
        return fail_finish ? Status::fault("read guard model injected finish fault") : status;
    }
    Status close()
    { return original_ ? budget_.close(*original_) : Status::not_initialized("read guard model is unbound"); }
    Status abort_original()
    { return original_ ? original_->abort(Status::fault("read guard model external abort")) : Status::not_initialized("read guard model unbound abort"); }
    fs::Ext4ReclaimRouteBudgetReport report() const { return budget_.report(); }
    usize begins{}, finishes{};
    bool change_geometry{}, refuse_begin{}, fail_finish{};
  private:
    Device &device_;
    Session *original_{};
    Budget budget_{};
};

struct DirectFixture
{
    Device device;
    Registry registry;
    Session session;
    Guard guard{device};
    std::array<std::byte, 1024> output{};
    Status open()
    {
        const auto key = registry.register_device(device, 5113, 4096);
        return key ? registry.acquire(key.value(), {4, 32}, block::SessionAccess::readonly, session) : key.status();
    }
};
struct CacheFixture
{
    Device device;
    Registry registry;
    fs::MetadataBuffers<1, 1024, 1> cache;
    Guard guard{device};
    Status open()
    {
        const auto key = registry.register_device(device, 5114, 4096);
        return key ? cache.open(registry, key.value(), {4, 32}, block::SessionAccess::readonly) : key.status();
    }
};
// Explicit model media: the first invalid superblock terminates actual public
// ChainCapture after one physical read. Large caller-owned storage is off-stack.
struct CaptureFixture
{
    Device device;
    Registry registry;
    Session session;
    Guard guard{device};
    fs::Ext4OrphanChainCapture<1> capture;
    fs::Ext4OrphanChainCaptureStorage storage;
    fs::Ext4OrphanDirectoryQualifiedReclaimScratch scratch;
    Status open()
    {
        device.sectors = 65536;
        const auto key = registry.register_device(device, 5115, 4096);
        return key ? registry.acquire(key.value(), {0, 65536}, block::SessionAccess::readonly, session) : key.status();
    }
};
// Actual public recovery Binding admission, stopped by an invalid model
// superblock. Its nested Capture and all caller-owned images are off-stack.
struct HistoryBindingFixture
{
    Device device;
    Registry registry;
    Session session;
    Guard guard{device};
    fs::Ext4ReclaimRecoveryBinding<1> binding;
    fs::Ext4ReclaimRecoveryStorage storage;
    fs::Ext4OrphanReclaimColdScratch scratch;
    Status open()
    {
        device.sectors = 65536;
        const auto key = registry.register_device(device, 5116, 4096);
        return key ? registry.acquire(key.value(), {0, 65536}, block::SessionAccess::readonly, session) : key.status();
    }
};

struct DirectAuthority
{
    Status operator()() const
    { return allowed ? Status{} : Status::denied("direct scope model authority changed"); }
    bool allowed{true};
};
struct DirectHook
{
    Status begin_read(Session &, u64, usize)
    {
        ++begins;
        return refuse_begin ? Status::overflow("direct scope model external begin refusal") : Status{};
    }
    Status finish_read(Session &, Status)
    {
        ++finishes;
        if (change_authority) authority.allowed = false;
        return refuse_finish ? Status::fault("direct scope model external finish refusal") : Status{};
    }
    DirectAuthority &authority;
    usize begins{}, finishes{};
    bool refuse_begin{}, refuse_finish{}, change_authority{};
};
// Real Session accounting with an independent Owner shadow, without ext4 media.
struct DirectScopeFixture
{
    using Scope = fs::Ext4DirectReadScope<Session, DirectAuthority>;
    Device device;
    Registry registry;
    Session session;
    block::SessionReport shadow{};
    DirectAuthority authority;
    DirectHook hook{authority};
    Scope *observed_scope{};
    usize observations{};
    std::array<std::byte, 1024> output{};
    Status open()
    {
        const auto key = registry.register_device(device, 5117, 4096);
        if (!key) return key.status();
        if (auto status = registry.acquire(key.value(), {4, 32}, block::SessionAccess::readonly, session); !status)
            return status;
        const auto initial = session.report(); if (!initial) return initial.status();
        shadow = initial.value();
        output.fill(std::byte{0xa5});
        device.context = this;
        device.observe = [](void *context) -> Status {
            auto &self = *static_cast<DirectScopeFixture *>(context);
            ++self.observations;
            if (self.observed_scope)
                if (auto status = self.observed_scope->in_flight_exact(self.session); !status) return status;
            const auto actual = self.session.report(); if (!actual) return actual.status();
            auto expected = self.shadow;
            ++expected.reads;
            expected.in_flight = true;
            return fs::reclaim_history::same_session(actual.value(), expected) ? Status{} :
                Status::fault("direct scope model paused Session or Owner prefix differs");
        };
        return {};
    }
};
} // namespace guarded_read_model

inline Status check_direct_read_scope_model()
{
    using namespace guarded_read_model;
    alignas(DirectScopeFixture) static std::array<std::byte, sizeof(DirectScopeFixture)> storage{};
    for (usize variant = 0; variant < 7; ++variant)
    {
        auto &fixture = *::new (storage.data()) DirectScopeFixture;
        const auto checked = [&]() -> Status {
            if (auto status = fixture.open(); !status) return status;
            const auto before = fixture.shadow;
            fixture.hook.refuse_begin = variant == 1;
            fixture.hook.refuse_finish = variant == 2 || variant == 3;
            fixture.hook.change_authority = variant == 5;
            if (variant == 3) fixture.device.failure = Status::io_error("direct scope model media failure");
            const u64 first = variant == 6 ? 1 : 2;
            auto bytes = std::span<std::byte>(fixture.output).first(variant == 6 ? 512 : 1024);
            Status dispatched;
            {
                DirectScopeFixture::Scope scope(fixture.session, fixture.shadow, fixture.authority, first, bytes.size());
                if (auto status = scope.prepare(); !status) return status;
                if (variant == 4)
                {
                    if (auto status = scope.begin_read(fixture.session, first, bytes.size()); !status) return status;
                }
                else
                {
                    fixture.observed_scope = &scope;
                    fs::Ext4DirectReadDispatch<decltype(scope), DirectHook, Session> dispatch{scope, fixture.hook};
                    dispatched = block::guarded_read_blocks(fixture.session, dispatch, first, bytes);
                    fixture.observed_scope = nullptr;
                    if (dispatched) dispatched = scope.adopt(fixture.session);
                }
            }
            const auto actual = fixture.session.report(); if (!actual) return actual.status();
            const auto &io = actual.value();
            const bool succeeded = variant == 0 || variant == 6;
            const bool attempted = variant != 1 && variant != 4;
            const bool completed = attempted && variant != 3;
            const auto expected = succeeded ? StatusCode::ok : variant == 1 ? StatusCode::overflow :
                variant == 3 ? StatusCode::io_error : variant == 4 ? StatusCode::interrupted :
                variant == 5 ? StatusCode::denied : StatusCode::fault;
            if ((variant != 4 && dispatched.code() != expected) ||
                fixture.hook.begins != usize(variant != 4) || fixture.hook.finishes != usize(attempted) ||
                fixture.device.reads != usize(attempted) || fixture.observations != usize(attempted) ||
                fixture.device.writes || fixture.device.flushes || io.reads != usize(attempted) ||
                io.completed_reads != usize(completed) || io.in_flight || io.pending_writes ||
                io.state != (succeeded || variant == 1 ? block::DeviceState::owned : block::DeviceState::quarantined) ||
                io.first_error != (succeeded || variant == 1 ? StatusCode::ok : expected))
                return Status::fault("direct scope model dispatch, destructor or priority accounting");
            if (!fs::reclaim_history::same_session(fixture.shadow, succeeded ? io : before))
                return Status::fault("direct scope model adopted failed or stale Owner prefix");
            for (usize index = 0; index < fixture.output.size(); ++index)
            {
                const auto expected_byte = completed && index < bytes.size() ? std::byte{0x5a} : std::byte{0xa5};
                if (fixture.output[index] != expected_byte)
                    return Status::fault("direct scope model output shape");
            }
            if (variant == 6)
            {
                // The old scope is gone. A fresh helper call borrows no old hook.
                if (auto status = fs::guarded_ext4_direct_read(fixture.session, fixture.shadow,
                        fixture.authority, 3, bytes, fixture.hook); !status) return status;
                const auto next = fixture.session.report(); if (!next) return next.status();
                if (next.value().reads != 2 || next.value().completed_reads != 2 ||
                    next.value().state != block::DeviceState::owned || next.value().first_error != StatusCode::ok ||
                    !fs::reclaim_history::same_session(fixture.shadow, next.value()) ||
                    fixture.hook.begins != 2 || fixture.hook.finishes != 2 || fixture.observations != 2 ||
                    fixture.device.reads != 2 || fixture.device.writes || fixture.device.flushes)
                    return Status::fault("direct scope model fresh call retained old scope or adopted twice");
            }
            return succeeded || variant == 1 ? fixture.session.close() : Status{};
        }();
        fixture.~DirectScopeFixture();
        if (!checked) return checked;
    }
    return {};
}

inline Status check_guarded_read_model()
{
    using namespace guarded_read_model;
    if (auto status = check_direct_read_scope_model(); !status) return status;
    alignas(DirectFixture) static std::array<std::byte, sizeof(DirectFixture)> direct_storage{};
    for (usize variant = 0; variant < 4; ++variant)
    {
        auto &fixture = *::new (direct_storage.data()) DirectFixture;
        const auto result = [&]() -> Status {
            if (auto status = fixture.open(); !status) return status;
            if (variant == 2) fixture.device.failure = Status::io_error("read guard model media failure");
            fixture.guard.change_geometry = variant == 3;
            const auto status = block::guarded_read_blocks(fixture.session, fixture.guard,
                variant == 1 ? 1 : 2, fixture.output);
            const auto current = fixture.session.report();
            if (!current) return current.status();
            const auto &io = current.value();
            const auto budget = fixture.guard.report();
            if (fixture.guard.begins != 1 || fixture.device.writes || fixture.device.flushes)
                return Status::fault("read guard model hook/write count");
            if (!variant)
            {
                if (!status || fixture.guard.finishes != 1 || fixture.device.reads != 1 ||
                    io.reads != 1 || io.completed_reads != 1 || io.state != block::DeviceState::owned ||
                    budget.attempts[0] != 1 || budget.completions[0] != 1)
                    return Status::fault("read guard model successful prefix");
                for (auto byte : fixture.output)
                    if (byte != std::byte{0x5a}) return Status::fault("read guard model output");
                if (auto closed = fixture.guard.close(); !closed) return closed;
                return fixture.session.close();
            }
            if (variant == 1)
            {
                if (status.code() != StatusCode::invalid_argument || fixture.guard.finishes ||
                    fixture.device.reads || io.reads || io.completed_reads ||
                    io.state != block::DeviceState::owned || budget.dispatch_pending)
                    return Status::fault("read guard model pre-dispatch refusal");
                if (auto closed = fixture.guard.close(); !closed) return closed;
                return fixture.session.close();
            }
            if (status.code() != StatusCode::io_error || fixture.guard.finishes != 1 ||
                fixture.device.reads != (variant == 2 ? 1U : 0U) || io.reads != 1 || io.completed_reads ||
                io.state != block::DeviceState::quarantined || io.first_error != StatusCode::io_error ||
                io.in_flight || io.pending_writes || budget.attempts[0] != 1 || budget.completions[0])
                return Status::fault("read guard model real failed prefix");
            return {};
        }();
        fixture.~DirectFixture();
        if (!result) return result;
    }
    alignas(CacheFixture) static std::array<std::byte, sizeof(CacheFixture)> cache_storage{};
    auto &fixture = *::new (cache_storage.data()) CacheFixture;
    const auto result = [&]() -> Status {
        if (auto status = fixture.open(); !status) return status;
        decltype(fixture.cache)::Pin first, second;
        if (auto status = fixture.cache.pin(0, first, fixture.guard); !status) return status;
        const auto before = first.report(); if (!before) return before.status();
        if (auto status = first.release(); !status) return status;
        if (auto status = fixture.guard.close(); !status) return status;
        if (auto status = fixture.cache.pin(0, second, fixture.guard); !status) return status;
        const auto after = second.report(); if (!after) return after.status();
        if (fixture.guard.begins != 1 || fixture.guard.finishes != 1 || fixture.device.reads != 1 ||
            fixture.device.writes || fixture.device.flushes || before.value().generation != after.value().generation ||
            after.value().read_pins != 1 || fixture.guard.report().attempts[0] != 1)
            return Status::fault("read guard model cache hit consumed closed budget");
        if (auto status = fixture.guard.abort_original(); !status) return status;
        std::array<std::byte, 16> output{};
        output.fill(std::byte{0xa5});
        if (second.read(0, output).code() != StatusCode::denied)
            return Status::fault("read guard model retained Pin exposed aborted Session");
        for (auto byte : output)
            if (byte != std::byte{0xa5}) return Status::fault("read guard model invalidated read changed output");
        decltype(fixture.cache)::Pin refused;
        if (fixture.cache.pin(0, refused, fixture.guard).code() != StatusCode::denied ||
            fixture.guard.begins != 1 || fixture.device.reads != 1 ||
            fixture.cache.report().state != fs::MetadataOwnerState::failed)
            return Status::fault("read guard model cache exposed aborted Session");
        return second.release();
    }();
    fixture.~CacheFixture();
    if (!result) return result;
    alignas(CaptureFixture) static std::array<std::byte, sizeof(CaptureFixture)> capture_storage{};
    for (usize variant = 0; variant < 4; ++variant)
    {
        auto &captured = *::new (capture_storage.data()) CaptureFixture;
        const auto checked = [&]() -> Status {
            if (auto status = captured.open(); !status) return status;
            captured.guard.refuse_begin = variant == 1;
            captured.guard.fail_finish = variant == 3;
            if (variant >= 2) captured.device.failure = Status::io_error("read guard model capture media failure");
            const auto status = captured.capture.capture(captured.session, captured.storage, captured.scratch, captured.guard);
            const auto current = captured.session.report(); if (!current) return current.status();
            const auto &io = current.value(); const bool refused = variant == 1;
            const auto expected = refused ? StatusCode::overflow : StatusCode::io_error;
            if (status.code() != expected || captured.capture.report().status.code() != expected ||
                captured.capture.report().phase != fs::Ext4OrphanSnapshotPhase::rejected || captured.capture.report().valid ||
                captured.guard.begins != 1 || captured.guard.finishes != usize(!refused) ||
                captured.device.reads != usize(!refused) || io.reads != usize(!refused) ||
                io.completed_reads != usize(!variant) || captured.capture.report().session_reads != usize(!variant) ||
                io.state != block::DeviceState::quarantined || io.first_error != expected ||
                io.in_flight || io.pending_writes || captured.device.writes || captured.device.flushes)
                return Status::fault("read guard model actual capture prefix or failure priority");
            return {};
        }();
        captured.~CaptureFixture();
        if (!checked) return checked;
    }
    alignas(HistoryBindingFixture) static std::array<std::byte, sizeof(HistoryBindingFixture)> history_storage{};
    for (usize variant = 0; variant < 4; ++variant)
    {
        auto &bound = *::new (history_storage.data()) HistoryBindingFixture;
        const auto checked = [&]() -> Status {
            if (auto status = bound.open(); !status) return status;
            bound.guard.refuse_begin = variant == 1;
            bound.guard.fail_finish = variant == 3;
            if (variant >= 2) bound.device.failure = Status::io_error("read guard history model media failure");
            const auto status = bound.binding.inspect(bound.session, bound.storage, bound.scratch, bound.guard);
            const auto current = bound.session.report(); if (!current) return current.status();
            const auto &io = current.value(); const bool refused = variant == 1;
            const auto expected = !variant ? StatusCode::unsupported : refused ? StatusCode::overflow : StatusCode::io_error;
            if (status.code() != expected || bound.binding.report().status.code() != expected ||
                bound.binding.report().phase != fs::Ext4ReclaimRecoveryPhase::rejected || bound.binding.report().valid ||
                bound.guard.begins != 1 || bound.guard.finishes != usize(!refused) ||
                bound.device.reads != usize(!refused) || io.reads != usize(!refused) ||
                io.completed_reads != usize(!variant) || bound.binding.report().reads != usize(!variant) ||
                io.state != block::DeviceState::quarantined || io.first_error != expected ||
                io.in_flight || io.pending_writes || bound.device.writes || bound.device.flushes)
                return Status::fault("read guard model actual history Binding prefix or failure priority");
            return {};
        }();
        bound.~HistoryBindingFixture();
        if (!checked) return checked;
    }
    return {};
}
} // namespace ok::tests
