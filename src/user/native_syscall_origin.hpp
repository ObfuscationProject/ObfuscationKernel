#pragma once

#include "ok/user/native_syscall_restart.hpp"
#include "ok/user/native_backend_capture.hpp"
#include "ok/user/native_child_wait.hpp"
#include "ok/user/native_pipe_vectors.hpp"
#include "ok/memory/kernel_entry_stack_pool.hpp"
#include "ok/memory/object_pool.hpp"
#include "ok/sched/scheduler.hpp"
#include "ok/syscall/syscall.hpp"
#include "ok/uapi/syscall.h"
#include "linux_udp_immediate_operation.hpp"
#include "ok/abi/linux_udp_call.hpp"
#include "tcp_immediate_operation.hpp"
#include "ok/user/unix_pathname_operation.hpp"

namespace ok::user::native
{

template <abi::linux_user::Profile Selected> class LinuxUdpReceiveCoordinator;

// Private runtime authority. No TCB, frame or resource pointer survives entry.
// All calls are serialized on the boot CPU; this is not an SMP protocol.
template <abi::linux_user::Profile Selected>
class SyscallOriginTable final : private NonCopyable
{
    friend class LinuxUdpReceiveCoordinator<Selected>;
    enum class Kind : u8 { none, linux_restart, native_udp_receive, linux_udp_immediate, linux_udp_receive, tcp_immediate };
  public:
    using Adapter = RestartAdapter<Selected>;
    using Frame = arch::execution::UserFrame;
    using Trap = arch::execution::Trap;
    using Space = arch::execution::AddressSpace;
    using Pool = memory::KernelEntryStackPool;
    using Handle = memory::KernelEntryStackHandle;
    static constexpr usize capacity = Pool::capacity;
    struct Identity
    {
        Handle lease{};
        u64 pid{}, tid{}, image{}, capture{}, epoch{};
        uptr lower{}, top{};
        Space space{};
        [[nodiscard]] bool operator==(const Identity &other) const
        {
            return lease == other.lease && pid == other.pid && tid == other.tid &&
                image == other.image && capture == other.capture && epoch == other.epoch &&
                lower == other.lower && top == other.top &&
                same_space(space, other.space);
        }
    };
    class TcpImmediateCapture final
    {
      public:
        TcpImmediateCapture(const TcpImmediateCapture &) = delete;
        TcpImmediateCapture &operator=(const TcpImmediateCapture &) = delete;
        TcpImmediateCapture &operator=(TcpImmediateCapture &&) = delete;
        TcpImmediateCapture(TcpImmediateCapture &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), identity_(other.identity_),
              operation_(other.operation_), syscall_(other.syscall_), linux_(other.linux_) {}
        ~TcpImmediateCapture() { reset(); }
        [[nodiscard]] bool live() const
        {
            if (!owner_) return false;
            if (owner_->resolve_tcp_immediate(*this)) return true;
            owner_->retire_tcp_immediate(*this); owner_ = nullptr; return false;
        }
        [[nodiscard]] u32 raw_syscall() const { return syscall_; }
        [[nodiscard]] u64 pid() const { return identity_.pid; }
        [[nodiscard]] u64 tid() const { return identity_.tid; }
        [[nodiscard]] abi::linux_user::TcpCall operation() const { return operation_; }
        [[nodiscard]] bool linux_personality() const { return linux_; }
        [[nodiscard]] Result<Space> address_space() const
        {
            if (!live()) return Status::denied("TCP immediate image is no longer current");
            return identity_.space;
        }
        void reset()
        {
            if (owner_) owner_->retire_tcp_immediate(*this);
            owner_ = nullptr;
        }
      private:
        friend class SyscallOriginTable;
        TcpImmediateCapture(SyscallOriginTable &owner, Identity identity,
            abi::linux_user::TcpCall operation, u32 number, bool linux)
            : owner_(&owner), identity_(identity), operation_(operation), syscall_(number), linux_(linux) {}
        mutable SyscallOriginTable *owner_{};
        Identity identity_{};
        abi::linux_user::TcpCall operation_{abi::linux_user::TcpCall::none};
        u32 syscall_{};
        bool linux_{};
    };
    class LinuxUdpImmediateCapture final
    {
      public:
        LinuxUdpImmediateCapture(const LinuxUdpImmediateCapture &) = delete;
        LinuxUdpImmediateCapture &operator=(const LinuxUdpImmediateCapture &) = delete;
        LinuxUdpImmediateCapture &operator=(LinuxUdpImmediateCapture &&) = delete;
        LinuxUdpImmediateCapture(LinuxUdpImmediateCapture &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), identity_(other.identity_),
              operation_(std::exchange(other.operation_, LinuxUdpImmediateOperation::none)),
              syscall_(std::exchange(other.syscall_, 0)) {}
        ~LinuxUdpImmediateCapture() { reset(); }
        [[nodiscard]] bool live() const
        {
            if (!owner_) return false;
            if (owner_->resolve_linux_udp_immediate(*this)) return true;
            owner_->retire_linux_udp_immediate(*this);
            owner_ = nullptr;
            return false;
        }
        [[nodiscard]] u64 raw_syscall() const { return syscall_; }
        [[nodiscard]] LinuxUdpImmediateOperation operation() const { return operation_; }
        [[nodiscard]] Result<Space> address_space() const
        {
            const auto *record = live() ? owner_->resolve_linux_udp_immediate(*this) : nullptr;
            if (!record) return Status::denied("Linux immediate image is no longer current");
            return record->identity.space;
        }
        void reset()
        {
            if (owner_) owner_->retire_linux_udp_immediate(*this);
            owner_ = nullptr;
        }
      private:
        friend class SyscallOriginTable;
        friend class LinuxUdpReceiveCoordinator<Selected>;
        LinuxUdpImmediateCapture(SyscallOriginTable &owner, Identity identity,
                                 LinuxUdpImmediateOperation operation, u32 number)
            : owner_(&owner), identity_(identity), operation_(operation), syscall_(number) {}
        mutable SyscallOriginTable *owner_{};
        Identity identity_{};
        LinuxUdpImmediateOperation operation_{LinuxUdpImmediateOperation::none};
        u32 syscall_{};
    };
    struct LinuxUdpReceiveBinding
    {
        memory::ObjectHandle object{};
        u64 request_sequence{};
        sched::WaitTicket ticket{};
        u16 description{sched::invalid_description};
        u32 endpoint{};
        abi::linux_user::UdpRequest operation{};
        [[nodiscard]] bool operator==(const LinuxUdpReceiveBinding &) const = default;
    };
    class LinuxUdpReceiveCapture final
    {
      public:
        LinuxUdpReceiveCapture(const LinuxUdpReceiveCapture &) = delete;
        LinuxUdpReceiveCapture &operator=(const LinuxUdpReceiveCapture &) = delete;
        LinuxUdpReceiveCapture &operator=(LinuxUdpReceiveCapture &&) = delete;
        LinuxUdpReceiveCapture(LinuxUdpReceiveCapture &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), identity_(other.identity_) {}
        ~LinuxUdpReceiveCapture() = default; // Exact retirement is explicit, never a destructor callback.
      private:
        friend class SyscallOriginTable;
        friend class LinuxUdpReceiveCoordinator<Selected>;
        LinuxUdpReceiveCapture(SyscallOriginTable &owner, Identity identity) noexcept
            : owner_(&owner), identity_(identity) {}
        SyscallOriginTable *owner_{};
        Identity identity_{};
    };
    // An ephemeral borrow has no raw owner/frame/PCB pointer. Retirement refuses
    // a live borrow; its destructor drops only the exact table borrow count.
    class LinuxUdpReceiveBorrow final
    {
      public:
        LinuxUdpReceiveBorrow(const LinuxUdpReceiveBorrow &) = delete;
        LinuxUdpReceiveBorrow &operator=(const LinuxUdpReceiveBorrow &) = delete;
        LinuxUdpReceiveBorrow &operator=(LinuxUdpReceiveBorrow &&) = delete;
        LinuxUdpReceiveBorrow(LinuxUdpReceiveBorrow &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), identity_(other.identity_), binding_(other.binding_) {}
        ~LinuxUdpReceiveBorrow() { reset(); }
        void reset()
        {
            if (auto *owner = std::exchange(owner_, nullptr)) owner->drop_linux_udp_borrow(identity_);
        }
        [[nodiscard]] bool live() const
        { return owner_ && owner_->current_linux_udp_access(identity_, binding_); }
        [[nodiscard]] Result<Space> address_space() const
        {
            if (!live()) return Status::denied("Linux UDP receive borrow lost current image");
            return identity_.space;
        }
      private:
        friend class SyscallOriginTable;
        LinuxUdpReceiveBorrow(SyscallOriginTable &owner, Identity identity, LinuxUdpReceiveBinding binding) noexcept
            : owner_(&owner), identity_(identity), binding_(binding) {}
        SyscallOriginTable *owner_{};
        Identity identity_{};
        LinuxUdpReceiveBinding binding_{};
    };
    struct NativeUdpReceiveRequest
    {
        i32 fd{};
        uptr output{};
        usize count{};
        uptr flags{};
        uptr source_address{}, source_length{};
        u32 source_capacity{};
        [[nodiscard]] constexpr bool operator==(const NativeUdpReceiveRequest &) const = default;
    };
    struct NativeUdpReceiveBinding
    {
        memory::ObjectHandle object{};
        sched::WaitTicket ticket{};
        u64 request{};
        u16 description{sched::invalid_description};
        u32 endpoint{};
        NativeUdpReceiveRequest operation{};
        [[nodiscard]] constexpr bool operator==(const NativeUdpReceiveBinding &) const = default;
    };
    enum class NativeUdpWaitState : u8 { pending, ready, cancelled, interrupted };
    class NativeUdpCapture final
    {
      public:
        NativeUdpCapture(const NativeUdpCapture &) = default;
        NativeUdpCapture &operator=(const NativeUdpCapture &) = default;
        [[nodiscard]] u64 pid() const { return identity_.pid; }
        [[nodiscard]] u64 tid() const { return identity_.tid; }
        [[nodiscard]] u64 sequence() const { return identity_.capture; }
      private:
        friend class SyscallOriginTable;
        NativeUdpCapture(const SyscallOriginTable *owner, Identity identity)
            : owner_(owner), identity_(identity) {}
        const SyscallOriginTable *owner_{};
        Identity identity_{};
    };
    struct PipeBinding
    {
        memory::ObjectHandle object{};
        sched::WaitTicket ticket{};
        u64 request{};
        i32 fd{};
        uptr output{};
        usize count{};
        u16 description{sched::invalid_description}, pipe{sched::invalid_description};
        PipeVectors vectors{};
        [[nodiscard]] constexpr bool operator==(const PipeBinding &) const = default;
    };
    struct PipeWriteBinding
    {
        memory::ObjectHandle object{};
        sched::WaitTicket ticket{};
        u64 request{};
        i32 fd{};
        uptr input{};
        usize count{};
        u16 description{sched::invalid_description}, pipe{sched::invalid_description};
        PipeVectors vectors{};
        usize committed{};
        [[nodiscard]] constexpr bool operator==(const PipeWriteBinding &) const = default;
    };
    struct FutexBinding
    {
        memory::ObjectHandle object{};
        sched::WaitTicket ticket{};
        uptr address{};
        u32 operation{}, expected{};
        u64 syscall{};
        uptr timeout_address{};
        u64 deadline{}, deadline_generation{};
        [[nodiscard]] constexpr bool operator==(const FutexBinding &) const = default;
    };
    struct ChildBinding
    {
        memory::ObjectHandle object{};
        sched::WaitTicket ticket{};
        u64 request{};
        ChildWaitRequest operation{};
        [[nodiscard]] constexpr bool operator==(const ChildBinding &) const = default;
    };
    template <typename Binding>
    static constexpr bool supported_binding = std::same_as<Binding, PipeBinding> || std::same_as<Binding, PipeWriteBinding> || std::same_as<Binding, FutexBinding> || std::same_as<Binding, ChildBinding> || std::same_as<Binding, LinuxUdpReceiveBinding>;
    template <typename Binding> requires supported_binding<Binding>
    class BoundRestartPermit
    {
      public:
        BoundRestartPermit() = default; // Invalid until minted by a live bound record.
        [[nodiscard]] const Identity &identity() const { return identity_; }
        [[nodiscard]] const Binding &binding() const { return binding_; }
        [[nodiscard]] const Frame &capture() const { return capture_; }
        [[nodiscard]] const Trap &trap() const { return trap_; }
        [[nodiscard]] const typename Adapter::Plan &plan() const { return plan_; }
      private:
        friend class SyscallOriginTable;
        BoundRestartPermit(const SyscallOriginTable *authority, Identity identity, Binding binding, const Frame &capture,
                      Trap trap, const typename Adapter::Plan &plan)
            : authority_(authority), identity_(identity), binding_(binding), capture_(capture), trap_(trap), plan_(plan) {}
        const SyscallOriginTable *authority_{};
        Identity identity_{};
        Binding binding_{};
        Frame capture_{};
        Trap trap_{};
        typename Adapter::Plan plan_{};
    };
    using RestartPermit = BoundRestartPermit<PipeBinding>;
    using PipeWriteRestartPermit = BoundRestartPermit<PipeWriteBinding>;
    using FutexRestartPermit = BoundRestartPermit<FutexBinding>;
    using ChildRestartPermit = BoundRestartPermit<ChildBinding>;
    using LinuxUdpRestartPermit = BoundRestartPermit<LinuxUdpReceiveBinding>;
    class SpaceUpdate
    {
      private:
        friend class SyscallOriginTable;
        SpaceUpdate(const SyscallOriginTable *owner, Space previous, Space next)
            : owner_(owner), previous_(previous), next_(next) {}
        const SyscallOriginTable *owner_{};
        Space previous_{}, next_{};
        std::array<u64, capacity> captures_{};
    };

    explicit constexpr SyscallOriginTable(Pool &pool) : pool_(&pool), legacy_capture_(pool) {}

    Status prepare(const sched::ThreadControlBlock &thread)
    {
        if (auto status = pool_->validate_inactive(thread.entry_stack, {thread.owner, thread.tid}); !status)
            return status;
        auto &record = records_[thread.entry_stack.slot];
        if (record.linux_udp_borrows) return Status::busy("syscall origin prepare has a live Linux UDP borrow");
        record = {};
        if (image_sequence_ == ~u64{0}) return Status::overflow("syscall origin image sequence exhausted");
        record.identity.lease = thread.entry_stack;
        record.identity.pid = thread.owner;
        record.identity.tid = thread.tid;
        record.identity.image = ++image_sequence_;
        return Status::success();
    }
    struct Ownership
    {
        usize prepared{}, live{}, bound{}, borrows{}, pending{};
    };
    [[nodiscard]] Ownership ownership() const
    {
        Ownership result{};
        for (const auto &record : records_)
        {
            result.prepared += record.identity.lease.pool != nullptr;
            result.live += record.live;
            result.bound += record.bound;
            result.borrows += record.linux_udp_borrows;
            result.pending += record.linux_udp_retire_pending || record.linux_udp_revoke_pending;
        }
        return result;
    }
    Status retire_checked(const sched::ThreadControlBlock &thread)
    {
        if (auto *record = owned(thread))
        {
            if (record->linux_udp_borrows)
                return Status::busy("syscall origin retirement has a live Linux UDP borrow");
            *record = {};
        }
        return {};
    }
    void retire(const sched::ThreadControlBlock &thread)
    {
        if (auto *record = owned(thread))
        {
            if (record->linux_udp_borrows) { record->linux_udp_retire_pending = true; return; }
            *record = {};
        }
    }
    void revoke(const sched::ThreadControlBlock &thread)
    {
        if (auto *record = owned(thread)) revoke_record(*record);
    }
    void revoke_process(sched::ProcessId pid)
    {
        for (auto &record : records_)
            if (record.identity.pid == pid) revoke_record(record);
    }
    void revoke_frame(const sched::ProcessControlBlock &process, const Frame &frame)
    {
        for (const auto &thread : process.threads())
            if (&thread.native_frame == &frame) { revoke(thread); return; }
    }

    class CaptureStage final
    {
      public:
        CaptureStage(const CaptureStage &) = delete;
        CaptureStage &operator=(const CaptureStage &) = delete;
        CaptureStage &operator=(CaptureStage &&) = delete;
        CaptureStage(CaptureStage &&other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), lease_(other.lease_), image_(std::exchange(other.image_, 0)), epoch_(other.epoch_) {}
      private:
        friend class SyscallOriginTable;
        CaptureStage(SyscallOriginTable &owner, Handle lease, u64 image, u64 epoch)
            : owner_(&owner), lease_(lease), image_(image), epoch_(epoch) {}
        SyscallOriginTable *owner_{};
        Handle lease_{};
        u64 image_{}, epoch_{};
    };
    Result<CaptureStage> stage_capture(sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                                      const arch::execution::KernelEntryStackView &view, sched::SchedulerState &scheduler)
    {
        auto *record = owned(thread);
        if (!record || !record->identity.image || process.pid() != thread.owner || !thread.native_active ||
            thread.syscall_pending || scheduler.find(process.pid()) != &process || scheduler.find_thread(thread.owner, thread.tid) != &thread)
            return Status::denied("syscall origin entry owner is not prepared");
        record->live = false; record->bound = false;
        const auto epoch = pool_->active_epoch(thread.entry_stack, {thread.owner, thread.tid}, view);
        if (!epoch) return epoch.status();
        return CaptureStage(*this, thread.entry_stack, record->identity.image, epoch.value());
    }
    Status observe(CaptureStage &stage, const BackendCapture::Observation &observation,
                   sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                   const arch::execution::KernelEntryStackView &view, sched::SchedulerState &scheduler)
    {
        namespace hw = arch::execution;
        auto *record = owned(thread);
        if (stage.owner_ != this || !stage.image_ || !record || record->identity.image != stage.image_ ||
            stage.lease_ != thread.entry_stack || stage.lease_ != observation.lease() || stage.epoch_ != observation.epoch() ||
            !BackendCapture::claim(observation, BackendCapture::Observer::restart_origin, scheduler, process, thread, view))
            return Status::denied("syscall origin has no shared owned capture stage");
        stage.owner_ = nullptr; stage.image_ = 0;
        if (observation.trap().kind != hw::TrapKind::syscall)
            return {};
        if (process.resources().personality == sched::UserPersonality::native)
        {
            const auto call = hw::syscall(observation.frame());
            if (native_unix_path_call(call, scheduler, process) != abi::linux_user::UnixPathCall::none)
                return {}; // Immediate UNIX owner pins before its first user copy.
            const auto tcp_operation = native_tcp_call(call, scheduler, process);
            if (tcp_operation != abi::linux_user::TcpCall::none && Adapter::syscall_trap(observation.trap()))
            {
                if (capture_sequence_ == ~u64{0}) return Status::overflow("TCP capture sequence exhausted");
                record->identity.capture = ++capture_sequence_;
                record->identity.epoch = stage.epoch_;
                record->identity.space = observation.space();
                record->identity.lower = view.lower(); record->identity.top = view.top();
                record->frame = observation.frame(); record->scheduler = &scheduler; record->trap = observation.trap();
                record->kind = Kind::tcp_immediate; record->family = Adapter::Family::none;
                record->tcp_operation = tcp_operation; record->tcp_syscall = static_cast<u32>(call.number);
                record->tcp_linux = false; record->tcp_wait = thread.wait_registration.ticket();
                record->tcp_dispatch = thread.dispatch_count; record->live = true;
                return {};
            }
            if (!Adapter::syscall_trap(observation.trap()) ||
                call.number != static_cast<uptr>(syscall::Number::recvfrom) ||
                call.arguments[0] > static_cast<uptr>(__INT32_MAX__) || call.arguments[3] ||
                bool(call.arguments[4]) != bool(call.arguments[5])) return {};
            if (capture_sequence_ == ~u64{0})
                return Status::overflow("syscall origin capture sequence exhausted");
            record->identity.capture = ++capture_sequence_;
            record->identity.epoch = stage.epoch_;
            record->identity.space = observation.space();
            record->identity.lower = view.lower();
            record->identity.top = view.top();
            record->frame = observation.frame();
            record->scheduler = &scheduler;
            record->trap = observation.trap();
            record->kind = Kind::native_udp_receive;
            record->family = Adapter::Family::none;
            record->live = true;
            return {};
        }
        if (process.resources().personality != sched::UserPersonality::linux) return {};
        if (capture_sequence_ == ~u64{0})
            return Status::overflow("syscall origin capture sequence exhausted");
        const u64 sequence = ++capture_sequence_;
        const auto raw_call = Adapter::Bridge::decode_call(observation.frame(), 0);
        if (linux_unix_path_call<Selected>(observation.frame(), scheduler, process) != abi::linux_user::UnixPathCall::none)
            return {}; // UNIX precedes TCP and UDP origin capture.
        const auto tcp_operation = linux_tcp_direct_call<Selected>(observation.frame(), scheduler, process);
        if (raw_call && tcp_operation != abi::linux_user::TcpCall::none && Adapter::syscall_trap(observation.trap()))
        {
            record->identity.capture = sequence; record->identity.epoch = stage.epoch_;
            record->identity.space = observation.space();
            record->identity.lower = view.lower(); record->identity.top = view.top();
            record->frame = observation.frame(); record->scheduler = &scheduler; record->trap = observation.trap();
            record->kind = Kind::tcp_immediate; record->family = Adapter::Family::none;
            // The immutable frame binds an effective accept/accept4 to its
            // exact outer socketcall number/selector on legacy profiles too.
            record->tcp_operation = tcp_operation; record->tcp_syscall = raw_call.value().number;
            record->tcp_linux = true; record->tcp_wait = thread.wait_registration.ticket();
            record->tcp_dispatch = thread.dispatch_count; record->live = true;
            return {};
        }
        const auto socket_operation = raw_call
            ? linux_udp_immediate_operation_for_frame<Selected>(observation.frame()) : LinuxUdpImmediateOperation::none;
        if (socket_operation != LinuxUdpImmediateOperation::none && Adapter::syscall_trap(observation.trap()))
        {
            record->identity.capture = sequence;
            record->identity.epoch = stage.epoch_;
            record->identity.space = observation.space();
            record->identity.lower = view.lower();
            record->identity.top = view.top();
            record->frame = observation.frame();
            record->scheduler = &scheduler;
            record->trap = observation.trap();
            record->kind = Kind::linux_udp_immediate;
            record->family = Adapter::Family::none;
            record->site = {};
            record->opcode = {};
            if (Adapter::family(observation.frame()) == Adapter::Family::linux_udp_receive)
            {
                const auto plan = Adapter::prepare_copy(observation.frame(), observation.trap(), hw::user_begin, hw::user_end,
                    [&](uptr address, std::span<u8> bytes) {
                        if (!executable(process.resources(), address, bytes.size()))
                            return Status::fault("Linux UDP restart opcode is not completely executable");
                        return hw::copy_from_user(observation.space(), bytes.data(), address, bytes.size());
                    });
                if (plan)
                {
                    record->family = Adapter::Family::linux_udp_receive;
                    record->site = plan.value().site;
                    record->opcode = plan.value().opcode;
                }
            }
            record->linux_udp_wait = thread.wait_registration.ticket();
            record->linux_udp_dispatch = thread.dispatch_count;
            record->linux_udp_operation = socket_operation;
            record->linux_udp_syscall = raw_call.value().number;
            record->live = true;
            return {};
        }
        // Unsupported instructions, other syscall families and MIPS BD keep normal
        // syscall behavior but obtain no restart authority.
        auto plan = Adapter::prepare_copy(observation.frame(), observation.trap(), hw::user_begin, hw::user_end,
            [&](uptr address, std::span<u8> bytes) {
                if (!executable(process.resources(), address, bytes.size()))
                    return Status::fault("syscall origin opcode is not completely executable");
                return hw::copy_from_user(observation.space(), bytes.data(), address, bytes.size());
            });
        if (!plan) return Status{};
        record->identity.capture = sequence;
        record->identity.epoch = stage.epoch_;
        record->identity.space = observation.space();
        record->identity.lower = view.lower();
        record->identity.top = view.top();
        record->frame = observation.frame();
        record->scheduler = &scheduler;
        record->trap = observation.trap();
        record->family = Adapter::family(observation.frame());
        record->kind = Kind::linux_restart;
        record->site = plan.value().site;
        record->opcode = plan.value().opcode;
        record->live = true;
        // n64/PPC observation consumption still occurs while this view is
        // active. Runtime revokes this unbound record if that validation fails.
        return {};
    }
    // Compatibility path for callers owning only the restart observer.
    Result<Trap> enter(sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
                       const arch::execution::KernelEntryStackView &view, sched::SchedulerState &scheduler)
    {
        auto stage = stage_capture(process, thread, view, scheduler);
        if (!stage) return stage.status();
        auto observed = legacy_capture_.enter(scheduler, process, thread, view);
        if (!observed) return observed.status();
        if (auto status = observe(stage.value(), observed.value(), process, thread, view, scheduler); !status) return status;
        return observed.value().trap();
    }

    [[nodiscard]] Identity identity(const sched::ProcessControlBlock &process,
                                     const sched::ThreadControlBlock &thread) const
    {
        const auto *record = owned(thread);
        if (!record || !record->live || record->kind != Kind::linux_restart ||
            process.resources().personality != sched::UserPersonality::linux ||
            process.pid() != thread.owner || !thread.native_active) return {};
        auto identity = record->identity;
        identity.space = process.resources().native_space;
        return identity;
    }
    [[nodiscard]] bool live(const Identity &identity) const { return resolve(identity) != nullptr; }

    template <typename Binding> requires (supported_binding<Binding> && !std::same_as<Binding, LinuxUdpReceiveBinding>)
    bool bind(const Identity &identity, const Frame &frame, const Binding &binding)
    {
        auto *record = resolve(identity);
        if (!record || record->kind != Kind::linux_restart || record->bound || record->family != binding_family<Binding>() ||
            !same_frame(record->frame, frame) || !valid_binding(identity, binding) ||
            !current_wait(*record, binding)) return false;
        if constexpr (std::same_as<Binding, PipeBinding>)
        {
            auto call = Adapter::Bridge::decode_call(record->frame, 3);
            const auto operation = Adapter::Codec::syscall_number(binding.vectors.count
                ? abi::linux_user::Operation::readv : abi::linux_user::Operation::read);
            if (!call || !operation || call.value().number != *operation ||
                static_cast<i32>(call.value().arguments[0]) != binding.fd || call.value().arguments[1] != binding.output)
                return false;
            if (binding.vectors.count)
            {
                const auto bytes = binding.vectors.staged_count(sched::pipe_capacity);
                if (call.value().arguments[2] != binding.vectors.count || !bytes || *bytes != binding.count) return false;
            }
            else if ((call.value().arguments[2] < sched::pipe_capacity ? call.value().arguments[2] : sched::pipe_capacity) != binding.count)
                return false;
        }
        else if constexpr (std::same_as<Binding, PipeWriteBinding>)
        {
            auto call = Adapter::Bridge::decode_call(record->frame, 3);
            const auto operation = Adapter::Codec::syscall_number(binding.vectors.count
                ? abi::linux_user::Operation::writev : abi::linux_user::Operation::write);
            if (!call || !operation || call.value().number != *operation ||
                static_cast<i32>(call.value().arguments[0]) != binding.fd || call.value().arguments[1] != binding.input)
                return false;
            if (binding.vectors.count)
            {
                const auto bytes = binding.vectors.staged_count(static_cast<usize>(__INTPTR_MAX__));
                if (call.value().arguments[2] != binding.vectors.count || !bytes || *bytes != binding.count) return false;
            }
            else if (call.value().arguments[2] != binding.count)
                return false;
        }
        else if constexpr (std::same_as<Binding, FutexBinding>)
        {
            const auto call = Adapter::Bridge::decode_call(record->frame, 4);
            if (!call || call.value().number != binding.syscall || call.value().arguments[0] != binding.address ||
                static_cast<u32>(call.value().arguments[1]) != binding.operation ||
                static_cast<u32>(call.value().arguments[2]) != binding.expected ||
                call.value().arguments[3] != binding.timeout_address) return false;
        }
        else
        {
            const auto &operation = binding.operation;
            const auto call = Adapter::Bridge::decode_call(record->frame, operation.siginfo ? 5 : 4);
            if (!call || operation.big_endian != Adapter::Codec::profile.big_endian) return false;
            const auto &a = call.value().arguments;
            if (operation.siginfo)
            {
                const auto number = Adapter::Codec::syscall_number(abi::linux_user::Operation::waitid);
                if (Selected != abi::linux_user::Profile::rv32 || !number || call.value().number != *number ||
                    static_cast<u32>(a[0]) != (operation.selector == -1 ? 0U : 1U) ||
                    (operation.selector != -1 && static_cast<u32>(a[1]) != static_cast<u32>(operation.selector)) ||
                    a[2] != operation.output || static_cast<u32>(a[3]) != 4 || a[4]) return false;
            }
            else
            {
                const auto number = Adapter::Codec::syscall_number(abi::linux_user::Operation::wait4);
                if (!number || call.value().number != *number || static_cast<i32>(a[0]) != operation.selector ||
                    a[1] != operation.output || static_cast<u32>(a[2]) || a[3]) return false;
            }
        }
        bound_record<Binding>(*record) = binding;
        record->bound = true;
        return true;
    }
    // Timed FUTEX_WAIT owns a captured continuation but cannot acquire a caught
    // handler restart permit. Completion authority is separate from restart.
    template <typename Binding> requires supported_binding<Binding>
    [[nodiscard]] bool validate_bound(const Identity &identity, const Frame &frame, const Binding &binding) const
    {
        if constexpr (std::same_as<Binding, LinuxUdpReceiveBinding>)
        {
            const auto *record = resolve_linux_udp_receive_identity(identity);
            return record && !record->linux_udp_borrows && record->linux_udp_receive_binding == binding &&
                same_frame(record->frame, frame) && binding.ticket.generation &&
                (current_linux_udp_wait(*record, binding, sched::WaitCompletion::none) ||
                 current_linux_udp_wait(*record, binding, sched::WaitCompletion::ready));
        }
        const auto *record = resolve(identity);
        return record && record->kind == Kind::linux_restart && record->bound && record->family == binding_family<Binding>() &&
            bound_record<Binding>(*record) == binding && same_frame(record->frame, frame);
    }
    template <typename Binding> requires (std::same_as<Binding, PipeBinding> || std::same_as<Binding, PipeWriteBinding> || std::same_as<Binding, ChildBinding>)
    bool rearm(const Identity &identity, const Frame &frame, const Binding &before, const Binding &after)
    {
        // Scheduler has just armed the new ticket; resolve the immutable owner
        // without requiring its already-replaced old wait to remain current.
        auto *record = resolve(identity, false);
        if (!record || record->kind != Kind::linux_restart || !record->bound || record->family != binding_family<Binding>() ||
            bound_record<Binding>(*record) != before || !same_frame(record->frame, frame) ||
            !valid_binding(identity, after) || after.ticket.generation <= before.ticket.generation)
            return false;
        auto unchanged = after;
        unchanged.ticket = before.ticket;
        if (unchanged != before || !current_wait(*record, after)) return false;
        bound_record<Binding>(*record) = after;
        return true;
    }
    [[nodiscard]] bool validate_cancelled_pipe_write(const Identity &identity, const Frame &frame,
                                                     const PipeWriteBinding &binding) const
    {
        const auto *record = resolve(identity, false);
        const auto *thread = record && record->scheduler ? record->scheduler->find_thread(identity.pid, identity.tid) : nullptr;
        return record && record->kind == Kind::linux_restart && record->bound && record->family == Adapter::Family::pipe_write &&
            record->write_binding == binding && same_frame(record->frame, frame) && thread && thread->syscall_pending &&
            thread->wait_registration.ticket() == binding.ticket && !thread->wait_registration.active() &&
            thread->wait_registration.completion() == sched::WaitCompletion::cancelled;
    }
    bool advance_pipe_write(const Identity &identity, const Frame &frame,
                            const PipeWriteBinding &before, const PipeWriteBinding &after)
    {
        auto *record = resolve(identity);
        if (!record || record->kind != Kind::linux_restart || !validate_bound(identity, frame, before) || !valid_binding(identity, after) ||
            after.committed <= before.committed) return false;
        auto unchanged = after;
        unchanged.committed = before.committed;
        if (unchanged != before) return false;
        record->write_binding = after;
        return true;
    }
    void retire_pipe(sched::ProcessId pid, sched::ThreadId tid, const PipeBinding &binding)
    { retire_binding(pid, tid, binding); }
    void retire_pipe_write(sched::ProcessId pid, sched::ThreadId tid, const PipeWriteBinding &binding)
    { retire_binding(pid, tid, binding); }
    void retire_child(sched::ProcessId pid, sched::ThreadId tid, const ChildBinding &binding)
    { retire_binding(pid, tid, binding); }
    void retire_futex(sched::ProcessId pid, sched::ThreadId tid, const FutexBinding &binding)
    { retire_binding(pid, tid, binding); }
    template <typename Binding> requires supported_binding<Binding>
    [[nodiscard]] Result<BoundRestartPermit<Binding>> prepare_restart(const Identity &identity, const Frame &frame,
                                                                     const Binding &binding) const
    {
        if (!validate_bound(identity, frame, binding))
            return Status::denied("syscall restart origin identity or wait changed");
        if constexpr (std::same_as<Binding, PipeWriteBinding>)
            if (binding.committed) return Status::unsupported("pipe write progress cannot restart after a handler");
        if constexpr (std::same_as<Binding, FutexBinding>)
            if (binding.timeout_address) return Status::unsupported("timed FUTEX_WAIT cannot restart after a handler");
        const auto *record = restart_record<Binding>(identity);
        if (record->family != binding_family<Binding>() || !record->site.bytes)
            return Status::unsupported("syscall restart has no captured opcode qualification");
        auto plan = Adapter::prepare(record->frame, record->trap,
            std::span<const u8>{record->opcode}.first(record->site.bytes),
            arch::execution::user_begin, arch::execution::user_end);
        if (!plan) return plan.status();
        return BoundRestartPermit<Binding>(this, identity, binding, record->frame, record->trap, plan.value());
    }
    template <typename Binding> requires supported_binding<Binding>
    [[nodiscard]] bool validate_restart(const BoundRestartPermit<Binding> &permit, const Identity &identity,
                                       const Frame &frame, const Binding &binding) const
    {
        const auto prepared = prepare_restart(identity, frame, binding);
        return prepared && permit.authority_ == this && permit.identity_ == identity && permit.binding_ == binding &&
            same_frame(permit.capture_, prepared.value().capture_) &&
            same_frame(permit.plan_.frame, prepared.value().plan_.frame) &&
            permit.plan_.opcode == prepared.value().plan_.opcode &&
            permit.plan_.site.instruction == prepared.value().plan_.site.instruction &&
            permit.plan_.site.bytes == prepared.value().plan_.site.bytes &&
            permit.plan_.site.thumb == prepared.value().plan_.site.thumb &&
            permit.plan_.ppc_result == prepared.value().plan_.ppc_result;
    }
    template <typename Binding> requires supported_binding<Binding>
    bool consume_restart(const BoundRestartPermit<Binding> &permit, const Identity &identity,
                         const Frame &frame, const Binding &binding)
    {
        if (!validate_restart(permit, identity, frame, binding)) return false;
        auto *record = const_cast<Record *>(restart_record<Binding>(identity));
        record->live = false;
        record->bound = false;
        return true;
    }

    [[nodiscard]] Result<TcpImmediateCapture> tcp_immediate_capture(
        const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
    {
        auto *record = owned(thread);
        if (!record || !record->live || record->kind != Kind::tcp_immediate || record->bound ||
            process.pid() != thread.owner || !record->scheduler ||
            record->scheduler->find(process.pid()) != &process ||
            record->scheduler->find_thread(process.pid(), thread.tid) != &thread)
            return Status::denied("TCP immediate capture is absent or already claimed");
        record->bound = true;
        TcpImmediateCapture capture{*this, record->identity, record->tcp_operation, record->tcp_syscall, record->tcp_linux};
        if (!capture.live()) return Status::denied("TCP immediate capture has no current image");
        return capture;
    }

    [[nodiscard]] bool has_tcp_immediate(const sched::ThreadControlBlock &thread) const
    {
        const auto *record = owned(thread);
        return record && record->live && !record->bound && record->kind == Kind::tcp_immediate;
    }

    [[nodiscard]] Result<LinuxUdpImmediateCapture> linux_udp_immediate_capture(
        const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
    {
        auto *record = owned(thread);
        if (!record || !record->live || record->kind != Kind::linux_udp_immediate || record->bound ||
            process.pid() != thread.owner || !record->scheduler ||
            record->scheduler->find(process.pid()) != &process ||
            record->scheduler->find_thread(process.pid(), thread.tid) != &thread)
            return Status::denied("Linux UDP immediate capture is absent or already claimed");
        record->bound = true;
        LinuxUdpImmediateCapture capture(*this, record->identity, record->linux_udp_operation, record->linux_udp_syscall);
        if (!capture.live()) return Status::denied("Linux UDP immediate capture has no current image");
        return capture;
    }

    [[nodiscard]] Status bind_linux_udp_receive(const LinuxUdpReceiveCapture &capture,
        const Frame &frame, const LinuxUdpReceiveBinding &binding)
    {
        auto *record = resolve_linux_udp_receive(capture);
        if (!record || record->linux_udp_borrows || record->linux_udp_receive_binding.ticket.generation ||
            !same_frame(record->frame, frame) || !valid_linux_udp_binding(record->identity, binding, true) ||
            binding.request_sequence != binding.ticket.generation ||
            !same_linux_udp_request(record->linux_udp_receive_binding, binding) ||
            !current_linux_udp_wait(*record, binding, sched::WaitCompletion::none))
            return Status::denied("Linux UDP receive bind lost staged authority");
        record->linux_udp_receive_binding = binding;
        return {};
    }
    [[nodiscard]] bool validate_linux_udp_receive(const LinuxUdpReceiveCapture &capture,
        const Frame &frame, const LinuxUdpReceiveBinding &binding, sched::WaitCompletion cause) const
    {
        const auto *record = resolve_linux_udp_receive(capture);
        return record && record->linux_udp_receive_binding == binding && same_frame(record->frame, frame) &&
            (binding.ticket.generation ? current_linux_udp_wait(*record, binding, cause)
                                      : current_linux_udp_access(capture.identity_, binding));
    }
    [[nodiscard]] Status rearm_linux_udp_receive(const LinuxUdpReceiveCapture &capture,
        const Frame &frame, const LinuxUdpReceiveBinding &before, const LinuxUdpReceiveBinding &after)
    {
        auto *record = resolve_linux_udp_receive(capture);
        auto unchanged = after; unchanged.ticket = before.ticket;
        if (!record || record->linux_udp_borrows || record->linux_udp_receive_binding != before ||
            !same_frame(record->frame, frame) || unchanged != before ||
            after.ticket.generation <= before.ticket.generation ||
            !valid_linux_udp_binding(record->identity, after, true) ||
            !current_linux_udp_wait(*record, after, sched::WaitCompletion::none))
            return Status::denied("Linux UDP receive rearm changed original authority");
        record->linux_udp_receive_binding = after;
        return {};
    }
    [[nodiscard]] Result<LinuxUdpReceiveBorrow> acquire_linux_udp_receive_access(
        const LinuxUdpReceiveCapture &capture, const LinuxUdpReceiveBinding &binding)
    {
        auto *record = resolve_linux_udp_receive(capture);
        if (!record || !current_linux_udp_access(capture.identity_, binding) || record->linux_udp_borrows == static_cast<u16>(~u16{0}))
            return Status::denied("Linux UDP receive has no current borrow authority");
        ++record->linux_udp_borrows;
        return LinuxUdpReceiveBorrow(*this, record->identity, binding);
    }
    [[nodiscard]] bool linux_udp_receive_borrowed(const LinuxUdpReceiveCapture &capture) const
    {
        const auto &identity = capture.identity_;
        if (capture.owner_ != this || identity.lease.pool != pool_ || identity.lease.slot >= capacity) return false;
        const auto &record = records_[identity.lease.slot];
        return same_native_udp_owner(record.identity, identity) && record.linux_udp_borrows;
    }
    [[nodiscard]] Status retire_linux_udp_receive(const LinuxUdpReceiveCapture &capture,
        const LinuxUdpReceiveBinding &binding)
    {
        const auto &identity = capture.identity_;
        if (capture.owner_ != this || identity.lease.pool != pool_ || identity.lease.slot >= capacity)
            return Status::denied("foreign Linux UDP receive capture");
        auto &record = records_[identity.lease.slot];
        if (record.kind != Kind::linux_udp_receive || !same_native_udp_owner(record.identity, identity) ||
            record.linux_udp_receive_binding != binding)
            return Status::denied("Linux UDP receive retirement lost exact origin");
        if (record.linux_udp_borrows) return Status::busy("Linux UDP receive retirement has a live borrow");
        record.live = false; record.bound = false;
        return {};
    }
    // Publication is one-shot and can only consume a current staged/ready owner.
    // No borrow survives this operation, including failure paths.
    [[nodiscard]] Status consume_linux_udp_receive(const LinuxUdpReceiveCapture &capture,
        const LinuxUdpReceiveBinding &binding)
    {
        auto *record = resolve_linux_udp_receive(capture);
        if (!record || record->linux_udp_borrows || !current_linux_udp_access(capture.identity_, binding))
            return Status::denied("Linux UDP receive publication lost exact authority");
        record->live = false; record->bound = false;
        return {};
    }

    // Recipient-side EINTR authority is separate from a current-image borrow
    // and from restart permits. A ready wake alone has not consumed a packet.
    [[nodiscard]] Status consume_linux_udp_receive_interruption(const LinuxUdpReceiveCapture &capture,
        const Frame &frame, const LinuxUdpReceiveBinding &binding)
    {
        auto *record = resolve_linux_udp_receive(capture);
        if (!record || record->linux_udp_borrows || !binding.ticket.generation ||
            record->linux_udp_receive_binding != binding || !same_frame(record->frame, frame) ||
            (!current_linux_udp_wait(*record, binding, sched::WaitCompletion::none) &&
             !current_linux_udp_wait(*record, binding, sched::WaitCompletion::ready)))
            return Status::denied("Linux UDP interruption lost exact published authority");
        record->live = false; record->bound = false;
        return {};
    }
    // This identity belongs only to the dedicated receive kind. Ordinary
    // identity()/resolve() remain unable to grant UDP restart authority.
    [[nodiscard]] Result<Identity> linux_udp_receive_identity(const LinuxUdpReceiveCapture &capture,
        const Frame &frame, const LinuxUdpReceiveBinding &binding) const
    {
        if (capture.owner_ != this || !validate_bound(capture.identity_, frame, binding))
            return Status::denied("Linux UDP restart identity has no current exact receive");
        return capture.identity_;
    }
    [[nodiscard]] Result<bool> linux_udp_receive_restart_qualified(const LinuxUdpReceiveCapture &capture,
        const Frame &frame, const LinuxUdpReceiveBinding &binding) const
    {
        const auto identity = linux_udp_receive_identity(capture, frame, binding);
        if (!identity) return identity.status();
        const auto *record = resolve_linux_udp_receive_identity(identity.value());
        return record->family == Adapter::Family::linux_udp_receive && record->site.bytes;
    }

    [[nodiscard]] Result<NativeUdpCapture> native_udp_capture(
        const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread) const
    {
        const auto *record = owned(thread);
        if (!record || record->kind != Kind::native_udp_receive || record->bound ||
            process.pid() != thread.owner || record->scheduler == nullptr ||
            record->scheduler->find(process.pid()) != &process ||
            record->scheduler->find_thread(process.pid(), thread.tid) != &thread ||
            record->scheduler->current_thread() != &thread ||
            thread.state != sched::ProcessState::running)
            return Status::denied("native UDP receive has no unbound captured owner");
        const NativeUdpCapture capture(this, record->identity);
        if (!resolve_native_udp(capture))
            return Status::denied("native UDP receive capture is not current");
        return capture;
    }
    Status bind_native_udp(const NativeUdpCapture &capture, const Frame &frame,
                           const NativeUdpReceiveBinding &binding)
    {
        auto *record = resolve_native_udp(capture);
        if (!record || record->bound || !same_frame(record->frame, frame) ||
            binding.request != binding.ticket.generation || !valid_native_udp_binding(record->identity, binding) ||
            !native_udp_operation_matches(record->frame, binding.operation) ||
            !current_native_udp_wait(*record, binding, NativeUdpWaitState::pending))
            return Status::denied("native UDP binding does not own the captured pending request");
        record->native_udp_binding = binding;
        record->bound = true;
        return {};
    }
    [[nodiscard]] bool validate_native_udp(const NativeUdpCapture &capture, const Frame &frame,
                                            const NativeUdpReceiveBinding &binding, NativeUdpWaitState state) const
    {
        const auto *record = resolve_native_udp(capture);
        return record && record->bound && record->native_udp_binding == binding &&
            same_frame(record->frame, frame) && current_native_udp_wait(*record, binding, state);
    }
    Status rearm_native_udp(const NativeUdpCapture &capture, const Frame &frame,
                            const NativeUdpReceiveBinding &before, const NativeUdpReceiveBinding &after)
    {
        auto *record = resolve_native_udp(capture);
        if (!record || !record->bound || record->native_udp_binding != before ||
            !same_frame(record->frame, frame) || !valid_native_udp_binding(record->identity, after) ||
            after.ticket.generation <= before.ticket.generation)
            return Status::denied("native UDP rearm lost its original captured binding");
        auto unchanged = after;
        unchanged.ticket = before.ticket;
        if (unchanged != before || !current_native_udp_wait(*record, after, NativeUdpWaitState::pending))
            return Status::denied("native UDP rearm changed its request or scheduler owner");
        record->native_udp_binding = after;
        return {};
    }
    void retire_native_udp(const NativeUdpCapture &capture, const NativeUdpReceiveBinding &binding)
    {
        if (capture.owner_ != this || capture.identity_.lease.pool != pool_ ||
            capture.identity_.lease.slot >= capacity) return;
        auto &record = records_[capture.identity_.lease.slot];
        if (record.kind == Kind::native_udp_receive && record.live && record.bound &&
            same_native_udp_owner(record.identity, capture.identity_) && record.native_udp_binding == binding)
        { record.live = false; record.bound = false; }
    }

    [[nodiscard]] Result<SpaceUpdate> stage_space(sched::SchedulerState *scheduler,
        const sched::ProcessResources &resources, Space previous, Space next) const
    {
        if (!next.root || !next.generation || same_space(previous, next) ||
            !same_space(resources.native_space, previous))
            return Status::invalid_argument("syscall origin space transaction is invalid");
        SpaceUpdate update(this, previous, next);
        for (usize slot = 0; slot < records_.size(); ++slot)
        {
            const auto &record = records_[slot];
            if (!record.live || !same_space(record.identity.space, previous)) continue;
            if (!current_owner(scheduler, resources, record, previous))
                return Status::denied("syscall origin space preflight lost owner");
            update.captures_[slot] = record.identity.capture;
        }
        return update;
    }
    bool commit_space(const SpaceUpdate &update, sched::SchedulerState *scheduler,
                       const sched::ProcessResources &resources)
    {
        if (update.owner_ != this || !same_space(resources.native_space, update.next_)) return false;
        // Validate every participant before changing any record.
        for (usize slot = 0; slot < records_.size(); ++slot)
        {
            const auto &record = records_[slot];
            const bool relevant = record.live && same_space(record.identity.space, update.previous_);
            if (relevant != (update.captures_[slot] != 0)) return false;
            if (relevant && (record.identity.capture != update.captures_[slot] ||
                !current_owner(scheduler, resources, record, update.next_))) return false;
        }
        for (usize slot = 0; slot < records_.size(); ++slot)
            if (update.captures_[slot]) records_[slot].identity.space = update.next_;
        return true;
    }

    [[nodiscard]] static bool same_frame(const Frame &a, const Frame &b)
    {
        return a.registers == b.registers && a.pc == b.pc && a.sp == b.sp && a.flags == b.flags &&
            a.fs_base == b.fs_base && a.gs_base == b.gs_base && a.fp_state == b.fp_state;
    }
    [[nodiscard]] static bool same_space(Space a, Space b)
    { return a.root == b.root && a.slot == b.slot && a.generation == b.generation; }

  private:
    struct Record
    {
        Identity identity{};
        Frame frame{};
        Trap trap{};
        typename Adapter::Site site{};
        std::array<u8, 4> opcode{};
        PipeBinding binding{};
        PipeWriteBinding write_binding{};
        FutexBinding futex_binding{};
        ChildBinding child_binding{};
        NativeUdpReceiveBinding native_udp_binding{};
        LinuxUdpReceiveBinding linux_udp_receive_binding{};
        u16 linux_udp_borrows{};
        bool linux_udp_revoke_pending{}, linux_udp_retire_pending{};
        LinuxUdpImmediateOperation linux_udp_operation{LinuxUdpImmediateOperation::none};
        u32 linux_udp_syscall{};
        sched::WaitTicket linux_udp_wait{};
        u64 linux_udp_dispatch{};
        abi::linux_user::TcpCall tcp_operation{abi::linux_user::TcpCall::none};
        u32 tcp_syscall{};
        sched::WaitTicket tcp_wait{};
        u64 tcp_dispatch{};
        bool tcp_linux{};
        Kind kind{Kind::none};
        typename Adapter::Family family{Adapter::Family::none};
        sched::SchedulerState *scheduler{};
        bool live{}, bound{};
    };
    static void revoke_record(Record &record)
    {
        if (record.linux_udp_borrows) record.linux_udp_revoke_pending = true;
        else { record.live = false; record.bound = false; }
    }
    template <typename Binding> requires supported_binding<Binding>
    [[nodiscard]] static constexpr typename Adapter::Family binding_family()
    {
        if constexpr (std::same_as<Binding, PipeBinding>) return Adapter::Family::pipe_read;
        else if constexpr (std::same_as<Binding, PipeWriteBinding>) return Adapter::Family::pipe_write;
        else if constexpr (std::same_as<Binding, FutexBinding>) return Adapter::Family::futex_wait;
        else if constexpr (std::same_as<Binding, LinuxUdpReceiveBinding>) return Adapter::Family::linux_udp_receive;
        else return Adapter::Family::child_wait;
    }
    template <typename Binding, typename RecordType> requires supported_binding<Binding>
    [[nodiscard]] static auto &bound_record(RecordType &record)
    {
        if constexpr (std::same_as<Binding, PipeBinding>) return record.binding;
        else if constexpr (std::same_as<Binding, PipeWriteBinding>) return record.write_binding;
        else if constexpr (std::same_as<Binding, FutexBinding>) return record.futex_binding;
        else if constexpr (std::same_as<Binding, LinuxUdpReceiveBinding>) return record.linux_udp_receive_binding;
        else return record.child_binding;
    }
    template <typename Binding> requires supported_binding<Binding>
    [[nodiscard]] const Record *restart_record(const Identity &identity) const
    {
        if constexpr (std::same_as<Binding, LinuxUdpReceiveBinding>)
            return resolve_linux_udp_receive_identity(identity);
        else return resolve(identity);
    }
    template <typename Binding> requires supported_binding<Binding>
    void retire_binding(sched::ProcessId pid, sched::ThreadId tid, const Binding &binding)
    {
        for (auto &record : records_)
            if (record.kind == Kind::linux_restart && record.live && record.bound && record.identity.pid == pid && record.identity.tid == tid &&
                record.family == binding_family<Binding>() && bound_record<Binding>(record) == binding)
            { record.live = false; record.bound = false; }
    }
    [[nodiscard]] static bool current_bound_wait(const Record &record)
    {
        if (record.kind == Kind::native_udp_receive)
            return current_native_udp_wait(record, record.native_udp_binding, NativeUdpWaitState::pending) ||
                current_native_udp_wait(record, record.native_udp_binding, NativeUdpWaitState::ready) ||
                current_native_udp_wait(record, record.native_udp_binding, NativeUdpWaitState::cancelled) ||
                current_native_udp_wait(record, record.native_udp_binding, NativeUdpWaitState::interrupted);
        if (record.kind == Kind::linux_udp_receive)
            return current_linux_udp_wait(record, record.linux_udp_receive_binding, sched::WaitCompletion::none) ||
                current_linux_udp_wait(record, record.linux_udp_receive_binding, sched::WaitCompletion::ready);
        if (record.kind != Kind::linux_restart) return false;
        switch (record.family)
        {
        case Adapter::Family::pipe_read: return current_wait(record, record.binding);
        case Adapter::Family::pipe_write: return current_wait(record, record.write_binding);
        case Adapter::Family::futex_wait: return current_wait(record, record.futex_binding);
        case Adapter::Family::child_wait: return current_wait(record, record.child_binding);
        default: return false;
        }
    }
    [[nodiscard]] const Record *owned(const sched::ThreadControlBlock &thread) const
    {
        if (thread.entry_stack.pool != pool_ || thread.entry_stack.slot >= capacity) return nullptr;
        const auto &record = records_[thread.entry_stack.slot];
        return record.identity.lease == thread.entry_stack && record.identity.pid == thread.owner &&
            record.identity.tid == thread.tid ? &record : nullptr;
    }
    Record *owned(const sched::ThreadControlBlock &thread)
    { return const_cast<Record *>(static_cast<const SyscallOriginTable *>(this)->owned(thread)); }
    [[nodiscard]] const Record *resolve(const Identity &identity, bool check_wait = true) const
    {
        if (identity.lease.pool != pool_ || identity.lease.slot >= capacity || !identity.capture ||
            !identity.epoch || !identity.image || !identity.pid || !identity.tid) return nullptr;
        const auto &record = records_[identity.lease.slot];
        const auto retained = pool_->inactive_epoch(identity.lease, {identity.pid, identity.tid});
        if (record.kind != Kind::linux_restart || !record.live || record.identity != identity || !retained || retained.value() != identity.epoch)
            return nullptr;
        const auto *process = record.scheduler ? record.scheduler->find(identity.pid) : nullptr;
        const auto *thread = record.scheduler ? record.scheduler->find_thread(identity.pid, identity.tid) : nullptr;
        if (!process || process->resources().personality != sched::UserPersonality::linux ||
            !thread || !thread->native_active || owned(*thread) != &record ||
            !same_space(process->resources().native_space, identity.space) ||
            !same_frame(thread->native_frame, record.frame) ||
            (check_wait && record.bound && !current_bound_wait(record))) return nullptr;
        return &record;
    }
    Record *resolve(const Identity &identity, bool check_wait = true)
    { return const_cast<Record *>(static_cast<const SyscallOriginTable *>(this)->resolve(identity, check_wait)); }
    [[nodiscard]] static bool same_native_udp_owner(const Identity &current, const Identity &captured)
    {
        return current.lease == captured.lease && current.pid == captured.pid && current.tid == captured.tid &&
            current.image == captured.image && current.capture == captured.capture && current.epoch == captured.epoch &&
            current.lower == captured.lower && current.top == captured.top;
    }
    [[nodiscard]] const Record *resolve_linux_udp_immediate(const LinuxUdpImmediateCapture &capture) const
    {
        const auto &identity = capture.identity_;
        if (capture.owner_ != this || identity.lease.pool != pool_ || identity.lease.slot >= capacity ||
            !identity.pid || !identity.tid || !identity.image || !identity.capture || !identity.epoch)
            return nullptr;
        const auto &record = records_[identity.lease.slot];
        const auto epoch = pool_->inactive_epoch(identity.lease, {identity.pid, identity.tid});
        if (!record.live || !record.bound || record.kind != Kind::linux_udp_immediate ||
            record.identity != identity || !epoch || epoch.value() != identity.epoch ||
            record.linux_udp_operation != capture.operation_ || record.linux_udp_syscall != capture.syscall_)
            return nullptr;
        const auto *process = record.scheduler ? record.scheduler->find(identity.pid) : nullptr;
        const auto *thread = record.scheduler ? record.scheduler->find_thread(identity.pid, identity.tid) : nullptr;
        if (!process || !thread || process->resources().personality != sched::UserPersonality::linux ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            !thread->native_active || !thread->syscall_pending || owned(*thread) != &record ||
            thread->state != sched::ProcessState::running || thread->wait_registration.active() ||
            thread->wait_registration.ticket() != record.linux_udp_wait ||
            thread->dispatch_count != record.linux_udp_dispatch ||
            record.scheduler->current_thread() != thread || record.scheduler->current_pid() != identity.pid ||
            !same_space(process->resources().native_space, identity.space) ||
            !same_frame(thread->native_frame, record.frame)) return nullptr;
        return &record;
    }
    [[nodiscard]] const Record *resolve_tcp_immediate(const TcpImmediateCapture &capture) const
    {
        const auto &identity = capture.identity_;
        if (capture.owner_ != this || identity.lease.pool != pool_ || identity.lease.slot >= capacity ||
            !identity.pid || !identity.tid || !identity.image || !identity.capture || !identity.epoch)
            return nullptr;
        const auto &record = records_[identity.lease.slot];
        const auto epoch = pool_->inactive_epoch(identity.lease, {identity.pid, identity.tid});
        if (!record.live || !record.bound || record.kind != Kind::tcp_immediate || record.identity != identity ||
            !epoch || epoch.value() != identity.epoch || record.tcp_operation != capture.operation_ ||
            record.tcp_syscall != capture.syscall_ || record.tcp_linux != capture.linux_)
            return nullptr;
        const auto *process = record.scheduler ? record.scheduler->find(identity.pid) : nullptr;
        const auto *thread = record.scheduler ? record.scheduler->find_thread(identity.pid, identity.tid) : nullptr;
        const auto personality = capture.linux_ ? sched::UserPersonality::linux : sched::UserPersonality::native;
        if (!process || !thread || process->resources().personality != personality ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            !thread->native_active || !thread->syscall_pending || owned(*thread) != &record ||
            thread->state != sched::ProcessState::running || thread->wait_registration.active() ||
            thread->wait_registration.ticket() != record.tcp_wait || thread->dispatch_count != record.tcp_dispatch ||
            record.scheduler->current_thread() != thread || record.scheduler->current_pid() != identity.pid ||
            !same_space(process->resources().native_space, identity.space) ||
            !same_frame(thread->native_frame, record.frame)) return nullptr;
        return &record;
    }
    void retire_tcp_immediate(const TcpImmediateCapture &capture)
    {
        const auto &identity = capture.identity_;
        if (capture.owner_ != this || identity.lease.pool != pool_ || identity.lease.slot >= capacity) return;
        auto &record = records_[identity.lease.slot];
        if (record.kind == Kind::tcp_immediate && record.bound && same_native_udp_owner(record.identity, identity) &&
            record.tcp_operation == capture.operation_ && record.tcp_syscall == capture.syscall_ &&
            record.tcp_linux == capture.linux_)
        { record.live = false; record.bound = false; }
    }
    void retire_linux_udp_immediate(const LinuxUdpImmediateCapture &capture)
    {
        const auto &identity = capture.identity_;
        if (capture.owner_ != this || identity.lease.pool != pool_ || identity.lease.slot >= capacity) return;
        auto &record = records_[identity.lease.slot];
        if (record.kind == Kind::linux_udp_immediate && record.bound &&
            same_native_udp_owner(record.identity, identity) && record.linux_udp_operation == capture.operation_ &&
            record.linux_udp_syscall == capture.syscall_)
        { record.live = false; record.bound = false; }
    }
    [[nodiscard]] static bool pure_linux_udp(const sched::OpenFileDescription *description, u32 endpoint)
    {
        return description && description->references && description->readable && endpoint &&
            description->native_udp_handle == endpoint && !description->native_tcp_handle &&
            !description->native_shared_file && !description->unix_endpoint &&
            description->unix_write_pipe == sched::invalid_description && !description->node &&
            description->pipe == sched::invalid_description && !description->console && !description->input_console;
    }
    [[nodiscard]] const Record *resolve_linux_udp_receive_identity(const Identity &identity) const
    {
        if (identity.lease.pool != pool_ || identity.lease.slot >= capacity || !identity.pid || !identity.tid ||
            !identity.image || !identity.capture || !identity.epoch) return nullptr;
        const auto &record = records_[identity.lease.slot];
        const auto epoch = pool_->inactive_epoch(identity.lease, {identity.pid, identity.tid});
        if (!record.live || !record.bound || record.linux_udp_revoke_pending || record.linux_udp_retire_pending ||
            record.kind != Kind::linux_udp_receive || record.identity != identity ||
            !epoch || epoch.value() != identity.epoch) return nullptr;
        const auto *process = record.scheduler ? record.scheduler->find(identity.pid) : nullptr;
        const auto *thread = record.scheduler ? record.scheduler->find_thread(identity.pid, identity.tid) : nullptr;
        if (!process || !thread || process->resources().personality != sched::UserPersonality::linux ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            !thread->native_active || !thread->syscall_pending || owned(*thread) != &record ||
            (thread->state != sched::ProcessState::running && thread->state != sched::ProcessState::runnable &&
             thread->state != sched::ProcessState::blocked) ||
            !same_space(process->resources().native_space, identity.space) ||
            !same_frame(thread->native_frame, record.frame) ||
            !pure_linux_udp(record.scheduler->open_description(record.linux_udp_receive_binding.description),
                            record.linux_udp_receive_binding.endpoint)) return nullptr;
        return &record;
    }
    [[nodiscard]] const Record *resolve_linux_udp_receive(const LinuxUdpReceiveCapture &capture) const
    { return capture.owner_ == this ? resolve_linux_udp_receive_identity(capture.identity_) : nullptr; }
    Record *resolve_linux_udp_receive(const LinuxUdpReceiveCapture &capture)
    { return const_cast<Record *>(static_cast<const SyscallOriginTable *>(this)->resolve_linux_udp_receive(capture)); }
    [[nodiscard]] static bool same_linux_udp_request(const LinuxUdpReceiveBinding &a, const LinuxUdpReceiveBinding &b)
    {
        return a.object == b.object && a.description == b.description && a.endpoint == b.endpoint && a.operation == b.operation;
    }
    [[nodiscard]] static bool valid_linux_udp_binding(const Identity &identity, const LinuxUdpReceiveBinding &binding, bool ticket)
    {
        const auto &operation = binding.operation;
        return binding.object.pool && binding.object.generation && binding.description != sched::invalid_description &&
            binding.endpoint && operation.call == abi::linux_user::UdpCall::recvfrom && operation.fd >= 0 &&
            !(operation.flags & ~abi::linux_user::UdpConstants<Selected>::receive_flags_mask()) &&
            !(operation.flags & *abi::linux_user::UdpConstants<Selected>::message(abi::linux_user::UdpMessage::dontwait)) &&
            (ticket ? binding.ticket.generation && binding.request_sequence &&
                binding.request_sequence <= binding.ticket.generation && binding.ticket.owner == identity.pid &&
                binding.ticket.thread == identity.tid : !binding.ticket.generation && !binding.request_sequence);
    }
    [[nodiscard]] static bool current_linux_udp_wait(const Record &record, const LinuxUdpReceiveBinding &binding,
        sched::WaitCompletion cause)
    {
        const auto *thread = record.scheduler ? record.scheduler->find_thread(record.identity.pid, record.identity.tid) : nullptr;
        if (!thread || thread->wait_registration.ticket() != binding.ticket || !binding.ticket.generation ||
            !valid_linux_udp_binding(record.identity, binding, true)) return false;
        if (cause == sched::WaitCompletion::none)
            return thread->wait_registration.pending(binding.ticket) && thread->state == sched::ProcessState::blocked &&
                thread->wait_reason == sched::ThreadWaitReason::udp_receive && thread->wait_key == binding.endpoint;
        return cause == sched::WaitCompletion::ready && !thread->wait_registration.active() &&
            thread->wait_registration.completion() == cause &&
            (thread->state == sched::ProcessState::running || thread->state == sched::ProcessState::runnable);
    }
    [[nodiscard]] bool current_linux_udp_access(const Identity &identity, const LinuxUdpReceiveBinding &binding) const
    {
        const auto *record = resolve_linux_udp_receive_identity(identity);
        const auto *thread = record && record->scheduler ? record->scheduler->find_thread(identity.pid, identity.tid) : nullptr;
        if (!record || !thread || record->linux_udp_receive_binding != binding ||
            record->scheduler->current_pid() != identity.pid || record->scheduler->current_thread() != thread ||
            thread->state != sched::ProcessState::running) return false;
        return binding.ticket.generation ? current_linux_udp_wait(*record, binding, sched::WaitCompletion::ready)
            : valid_linux_udp_binding(identity, binding, false) && !thread->wait_registration.active() &&
                thread->wait_registration.ticket() == record->linux_udp_wait && thread->dispatch_count == record->linux_udp_dispatch;
    }
    void drop_linux_udp_borrow(const Identity &identity)
    {
        // Exact revocation/retirement refuses a nonzero count. This cannot access
        // a revoked record or an object/description on a normal or failed exit.
        auto &record = records_[identity.lease.slot];
        if (record.kind == Kind::linux_udp_receive && record.live && record.identity == identity && record.linux_udp_borrows)
        {
            --record.linux_udp_borrows;
            if (!record.linux_udp_borrows)
            {
                if (record.linux_udp_retire_pending) record = {};
                else if (record.linux_udp_revoke_pending) { record.live = false; record.bound = false; }
            }
        }
    }
    [[nodiscard]] const Record *resolve_native_udp(const NativeUdpCapture &capture) const
    {
        const auto &identity = capture.identity_;
        if (capture.owner_ != this || identity.lease.pool != pool_ || identity.lease.slot >= capacity ||
            !identity.pid || !identity.tid || !identity.image || !identity.capture || !identity.epoch)
            return nullptr;
        const auto &record = records_[identity.lease.slot];
        const auto retained = pool_->inactive_epoch(identity.lease, {identity.pid, identity.tid});
        if (!record.live || record.kind != Kind::native_udp_receive ||
            !same_native_udp_owner(record.identity, identity) || !retained || retained.value() != identity.epoch)
            return nullptr;
        const auto *process = record.scheduler ? record.scheduler->find(identity.pid) : nullptr;
        const auto *thread = record.scheduler ? record.scheduler->find_thread(identity.pid, identity.tid) : nullptr;
        if (!process || process->resources().personality != sched::UserPersonality::native ||
            process->state() == sched::ProcessState::zombie || process->state() == sched::ProcessState::exited ||
            !thread || !thread->native_active || !thread->syscall_pending || owned(*thread) != &record ||
            (thread->state != sched::ProcessState::running && thread->state != sched::ProcessState::runnable &&
             thread->state != sched::ProcessState::blocked) ||
            !same_space(process->resources().native_space, record.identity.space) ||
            !same_frame(thread->native_frame, record.frame)) return nullptr;
        return &record;
    }
    Record *resolve_native_udp(const NativeUdpCapture &capture)
    { return const_cast<Record *>(static_cast<const SyscallOriginTable *>(this)->resolve_native_udp(capture)); }
    [[nodiscard]] static bool native_udp_operation_matches(const Frame &frame, const NativeUdpReceiveRequest &operation)
    {
        const auto call = arch::execution::syscall(frame);
        const auto &a = call.arguments;
        return call.number == static_cast<uptr>(syscall::Number::recvfrom) && operation.fd >= 0 &&
            a[0] == static_cast<uptr>(operation.fd) && a[1] == operation.output && a[2] == operation.count &&
            a[3] == operation.flags && !operation.flags && a[4] == operation.source_address &&
            a[5] == operation.source_length;
    }
    [[nodiscard]] static bool valid_native_udp_binding(const Identity &identity, const NativeUdpReceiveBinding &binding)
    {
        const auto &operation = binding.operation;
        return binding.object.pool && binding.object.generation && binding.ticket.generation && binding.request &&
            binding.request <= binding.ticket.generation && binding.ticket.owner == identity.pid &&
            binding.ticket.thread == identity.tid && binding.description != sched::invalid_description &&
            binding.endpoint && operation.fd >= 0 && !operation.flags &&
            bool(operation.source_address) == bool(operation.source_length) &&
            (operation.source_address ? operation.source_capacity >= sizeof(ok_sockaddr_in) : !operation.source_capacity);
    }
    [[nodiscard]] static bool current_native_udp_wait(const Record &record, const NativeUdpReceiveBinding &binding,
                                                      NativeUdpWaitState state)
    {
        const auto *thread = record.scheduler ? record.scheduler->find_thread(record.identity.pid, record.identity.tid) : nullptr;
        const auto *description = record.scheduler ? record.scheduler->open_description(binding.description) : nullptr;
        if (!thread || !thread->native_active || !thread->syscall_pending ||
            thread->wait_registration.ticket() != binding.ticket || !description || !description->references ||
            !description->readable || description->native_udp_handle != binding.endpoint) return false;
        if (state == NativeUdpWaitState::pending)
            return thread->wait_registration.pending(binding.ticket) && thread->state == sched::ProcessState::blocked &&
                thread->wait_reason == sched::ThreadWaitReason::udp_receive && thread->wait_key == binding.endpoint;
        if (thread->wait_registration.active() ||
            (thread->state != sched::ProcessState::runnable && thread->state != sched::ProcessState::running)) return false;
        switch (state)
        {
        case NativeUdpWaitState::ready: return thread->wait_registration.completion() == sched::WaitCompletion::ready;
        case NativeUdpWaitState::cancelled: return thread->wait_registration.completion() == sched::WaitCompletion::cancelled;
        case NativeUdpWaitState::interrupted: return thread->wait_registration.completion() == sched::WaitCompletion::interrupted;
        default: return false;
        }
    }
    [[nodiscard]] static bool current_wait(const Record &record, const PipeBinding &binding)
    {
        const auto *thread = record.scheduler ? record.scheduler->find_thread(record.identity.pid, record.identity.tid) : nullptr;
        const auto *description = record.scheduler ? record.scheduler->open_description(binding.description) : nullptr;
        if (!thread || !thread->syscall_pending || thread->wait_registration.ticket() != binding.ticket ||
            !description || !description->readable || description->pipe != binding.pipe || !description->references)
            return false;
        return thread->wait_registration.active()
            ? thread->state == sched::ProcessState::blocked && thread->wait_reason == sched::ThreadWaitReason::pipe_read &&
                thread->wait_key == binding.pipe
            : thread->wait_registration.completion() == sched::WaitCompletion::ready;
    }
    [[nodiscard]] static bool valid_binding(const Identity &identity, const PipeBinding &binding)
    {
        return binding.object.pool && binding.ticket.generation && binding.request &&
            binding.request <= binding.ticket.generation && binding.ticket.owner == identity.pid &&
            binding.ticket.thread == identity.tid && binding.count && binding.count <= sched::pipe_capacity &&
            binding.description != sched::invalid_description && binding.pipe != sched::invalid_description;
    }
    [[nodiscard]] static bool current_wait(const Record &record, const PipeWriteBinding &binding)
    {
        const auto *thread = record.scheduler ? record.scheduler->find_thread(record.identity.pid, record.identity.tid) : nullptr;
        const auto *description = record.scheduler ? record.scheduler->open_description(binding.description) : nullptr;
        if (!thread || !thread->syscall_pending || thread->wait_registration.ticket() != binding.ticket ||
            !description || !description->writable || description->pipe != binding.pipe || !description->references)
            return false;
        return thread->wait_registration.active()
            ? thread->state == sched::ProcessState::blocked && thread->wait_reason == sched::ThreadWaitReason::pipe_write &&
                thread->wait_key == binding.pipe
            : thread->wait_registration.completion() == sched::WaitCompletion::ready;
    }
    [[nodiscard]] static bool valid_binding(const Identity &identity, const PipeWriteBinding &binding)
    {
        return binding.object.pool && binding.ticket.generation && binding.request &&
            binding.request <= binding.ticket.generation && binding.ticket.owner == identity.pid &&
            binding.ticket.thread == identity.tid && binding.count && binding.count <= static_cast<usize>(__INTPTR_MAX__) &&
            binding.committed <= binding.count &&
            binding.description != sched::invalid_description && binding.pipe != sched::invalid_description;
    }
    [[nodiscard]] static bool current_wait(const Record &record, const FutexBinding &binding)
    {
        const auto *thread = record.scheduler ? record.scheduler->find_thread(record.identity.pid, record.identity.tid) : nullptr;
        return thread && thread->syscall_pending && thread->state == sched::ProcessState::blocked &&
            thread->wait_reason == sched::ThreadWaitReason::futex && thread->wait_key == binding.address &&
            thread->wait_registration.pending(binding.ticket);
    }
    [[nodiscard]] static bool valid_binding(const Identity &identity, const FutexBinding &binding)
    {
        namespace hw = arch::execution;
        if (!binding.object.pool || !binding.ticket.generation || binding.ticket.owner != identity.pid ||
            binding.ticket.thread != identity.tid || binding.address < hw::user_begin ||
            binding.address > hw::user_end - sizeof(u32) || binding.address % sizeof(u32) ||
            (binding.operation != 0 && binding.operation != 128)) return false;
        const auto futex = Adapter::Codec::syscall_number(abi::linux_user::Operation::futex);
        const auto time64 = Adapter::Codec::syscall_number(abi::linux_user::Operation::futex_time64);
        if ((!futex || binding.syscall != *futex) && (!time64 || binding.syscall != *time64)) return false;
        return binding.timeout_address ? binding.deadline && binding.deadline_generation
                                       : !binding.deadline && !binding.deadline_generation;
    }
    [[nodiscard]] static bool current_wait(const Record &record, const ChildBinding &binding)
    {
        const auto *thread = record.scheduler ? record.scheduler->find_thread(record.identity.pid, record.identity.tid) : nullptr;
        if (!thread || !thread->syscall_pending || thread->wait_registration.ticket() != binding.ticket) return false;
        return thread->wait_registration.active()
            ? thread->state == sched::ProcessState::blocked && thread->wait_reason == sched::ThreadWaitReason::child &&
                thread->wait_key == (binding.operation.selector == -1 ? 0 : static_cast<u32>(binding.operation.selector))
            : thread->wait_registration.completion() == sched::WaitCompletion::ready;
    }
    [[nodiscard]] static bool valid_binding(const Identity &identity, const ChildBinding &binding)
    {
        return binding.object.pool && binding.ticket.generation && binding.request &&
            binding.request <= binding.ticket.generation && binding.ticket.owner == identity.pid &&
            binding.ticket.thread == identity.tid && !binding.operation.nohang &&
            (binding.operation.selector == -1 || binding.operation.selector > 0);
    }
    [[nodiscard]] bool current_owner(sched::SchedulerState *scheduler,
        const sched::ProcessResources &resources, const Record &record, Space space) const
    {
        if (!scheduler || scheduler != record.scheduler) return false;
        const auto retained = pool_->inactive_epoch(record.identity.lease, {record.identity.pid, record.identity.tid});
        if (!retained || retained.value() != record.identity.epoch) return false;
        const auto *process = scheduler->find(record.identity.pid);
        const auto *thread = scheduler->find_thread(record.identity.pid, record.identity.tid);
        return process && thread && &process->resources() == &resources && thread->native_active &&
            ((record.kind == Kind::linux_restart && resources.personality == sched::UserPersonality::linux) ||
             (record.kind == Kind::native_udp_receive && resources.personality == sched::UserPersonality::native)) &&
            owned(*thread) == &record && same_space(process->resources().native_space, space) &&
            same_frame(thread->native_frame, record.frame) &&
            pool_->validate_inactive(thread->entry_stack, {thread->owner, thread->tid}).ok() &&
            (!record.bound || current_bound_wait(record));
    }
    [[nodiscard]] static bool executable(const sched::ProcessResources &resources, uptr address, usize bytes)
    {
        namespace hw = arch::execution;
        if (!bytes || address < hw::user_begin || address >= hw::user_end || bytes > hw::user_end - address)
            return false;
        const uptr end = address + bytes;
        for (uptr page = address & ~(hw::page_size - 1); page < end; page += hw::page_size)
        {
            bool found = false;
            for (const auto &mapping : resources.native_pages)
                if (mapping.virtual_address == page && (mapping.flags & hw::page_execute)) { found = true; break; }
            if (!found) return false;
        }
        return hw::user_range(resources.native_space, address, bytes);
    }
#ifdef OK_SYSCALL_ORIGIN_MODEL
    friend struct SyscallOriginTestAccess;
#endif
    Pool *pool_{};
    BackendCapture legacy_capture_;
    std::array<Record, capacity> records_{};
    u64 image_sequence_{}, capture_sequence_{};
};

} // namespace ok::user::native
