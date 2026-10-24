#pragma once

#include "linux_udp_immediate_access.hpp"
#include "linux_udp_original_ofd.hpp"
#include "linux_udp_captured_decode.hpp"
#include "linux_udp_receive_order.hpp"
#include "ok/core/kernel.hpp"

namespace ok::user::native
{
namespace linux_udp { class OperationScratch; }
template <abi::linux_user::Profile Selected, memory::ObjectPageProvider Provider> class LinuxUdpReceiveOwnerStore;
enum class LinuxUdpReceivePhase : u8 { object_staged, captured_staged, published, taken };
enum class LinuxUdpReceiveBeginKind : u8 { immediate, deferred, owned_completed };
struct LinuxUdpReceiveBegin
{
    LinuxUdpReceiveBeginKind kind;
    std::optional<i64> target_result;
};
enum class LinuxUdpReceiveRetirement : u8 { completed, rollback, process_exit, thread_exit, exec };

template <abi::linux_user::Profile Selected>
class LinuxUdpReceiveAdmission final
{
  public:
    LinuxUdpReceiveAdmission(const LinuxUdpReceiveAdmission &) = delete;
    LinuxUdpReceiveAdmission &operator=(const LinuxUdpReceiveAdmission &) = delete;
    LinuxUdpReceiveAdmission &operator=(LinuxUdpReceiveAdmission &&) = delete;
    LinuxUdpReceiveAdmission(LinuxUdpReceiveAdmission &&other) noexcept
        : issuer_(std::exchange(other.issuer_, nullptr)), identity_(other.identity_), operation_(other.operation_),
          description_(other.description_), endpoint_(other.endpoint_) {}
  private:
    friend class LinuxUdpReceiveCoordinator<Selected>;
    using Origins = SyscallOriginTable<Selected>;
    LinuxUdpReceiveAdmission(Origins *issuer, typename Origins::Identity identity,
        abi::linux_user::UdpRequest operation, u16 description, u32 endpoint) noexcept
        : issuer_(issuer), identity_(identity), operation_(operation), description_(description), endpoint_(endpoint) {}
    Origins *issuer_{};
    typename Origins::Identity identity_{};
    abi::linux_user::UdpRequest operation_{};
    u16 description_{sched::invalid_description};
    u32 endpoint_{};
};

template <abi::linux_user::Profile Selected>
struct LinuxUdpReceivePreparation
{
    std::optional<LinuxUdpReceiveAdmission<Selected>> admission;
    std::optional<i64> target_result;
};

template <abi::linux_user::Profile Selected>
[[nodiscard]] i64 linux_udp_receive_errno(abi::linux_user::UdpErrno value)
{ return -static_cast<i64>(*abi::linux_user::UdpConstants<Selected>::errno_number(value)); }
template <abi::linux_user::Profile Selected>
[[nodiscard]] i64 linux_udp_receive_status(Status status)
{
    using E = abi::linux_user::UdpErrno;
    switch (status.code())
    {
    case StatusCode::fault: return linux_udp_receive_errno<Selected>(E::e_fault);
    case StatusCode::invalid_argument: return linux_udp_receive_errno<Selected>(E::e_inval);
    case StatusCode::no_memory: return linux_udp_receive_errno<Selected>(E::e_nomem);
    case StatusCode::process_file_limit: return linux_udp_receive_errno<Selected>(E::e_mfile);
    case StatusCode::system_file_limit: return linux_udp_receive_errno<Selected>(E::e_nfile);
    case StatusCode::would_block: return linux_udp_receive_errno<Selected>(E::e_again);
    case StatusCode::unsupported: return linux_udp_receive_errno<Selected>(E::e_opnotsupp);
    case StatusCode::not_initialized: return linux_udp_receive_errno<Selected>(E::e_netdown);
    case StatusCode::message_size: return linux_udp_receive_errno<Selected>(E::e_msgsize);
    case StatusCode::overflow: return linux_udp_receive_errno<Selected>(E::e_overflow);
    case StatusCode::bad_descriptor: return linux_udp_receive_errno<Selected>(E::e_badf);
    default: return linux_udp_receive_errno<Selected>(E::e_io);
    }
}

template <abi::linux_user::Profile Selected>
struct LinuxUdpReceiveOwner
{
    using Origins = SyscallOriginTable<Selected>;
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    LinuxUdpReceivePhase phase{LinuxUdpReceivePhase::object_staged};
    std::optional<typename Origins::LinuxUdpReceiveCapture> capture;
    abi::linux_user::UdpRequest operation{};
    u32 endpoint{};
    u16 retained_description{sched::invalid_description};
    sched::WaitTicket ticket{};
    u64 request_sequence{}, observed_receive{}, observed_executor{};
    std::optional<typename Origins::LinuxUdpReceiveBinding> binding;
    LinuxUdpReceiveOwner(sched::ProcessId process, sched::ThreadId thread) noexcept : pid(process), tid(thread) {}
    LinuxUdpReceiveOwner(const LinuxUdpReceiveOwner &) = delete;
    LinuxUdpReceiveOwner &operator=(const LinuxUdpReceiveOwner &) = delete;
    LinuxUdpReceiveOwner &operator=(LinuxUdpReceiveOwner &&) = delete;
    LinuxUdpReceiveOwner(LinuxUdpReceiveOwner &&) noexcept = default;
};

// Narrow private handoff. Neither copied requests nor synthetic operation seams
// can construct admission, transfer or captured receive authority.
template <abi::linux_user::Profile Selected>
class LinuxUdpReceiveCoordinator final
{
    using Origins = SyscallOriginTable<Selected>;
    using Access = LinuxUdpImmediateAccess<Selected>;
    using Admission = LinuxUdpReceiveAdmission<Selected>;
    using Owner = LinuxUdpReceiveOwner<Selected>;
    friend class linux_udp::OperationScratch;
    template <abi::linux_user::Profile Target, memory::ObjectPageProvider Provider> friend class LinuxUdpReceiveOwnerStore;
  private:
    [[nodiscard]] static Status validate_store(Origins &issuer, Kernel &kernel,
        sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread, Access &access,
        const Admission &admission, const linux_udp::OriginalUdpDescription &original)
    {
        if (admission.issuer_ != &issuer || &original.scheduler_ != &kernel.scheduler() ||
            kernel.scheduler().find(process.pid()) != &process ||
            kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
            kernel.scheduler().current_thread() != &thread || process.pid() != thread.owner)
            return Status::denied("Linux UDP store requires its canonical issuer and current scheduler owner");
        return validate_admission(access, admission, original, process.pid(), thread.tid);
    }
    class Transfer final
    {
      public:
        Transfer(const Transfer &) = delete;
        Transfer &operator=(const Transfer &) = delete;
        Transfer &operator=(Transfer &&) = delete;
        Transfer(Transfer &&other) noexcept : issuer_(std::exchange(other.issuer_, nullptr)),
            identity_(other.identity_), object_(other.object_), operation_(other.operation_),
            description_(other.description_), endpoint_(other.endpoint_) {}
      private:
        friend class LinuxUdpReceiveCoordinator;
        Transfer(const Admission &admission, memory::ObjectHandle object) noexcept
            : issuer_(admission.issuer_), identity_(admission.identity_), object_(object), operation_(admission.operation_),
              description_(admission.description_), endpoint_(admission.endpoint_) {}
        Origins *issuer_{};
        typename Origins::Identity identity_{};
        memory::ObjectHandle object_{};
        abi::linux_user::UdpRequest operation_{};
        u16 description_{sched::invalid_description};
        u32 endpoint_{};
    };
    [[nodiscard]] static Status validate_admission(Access &access, const Admission &admission,
        const linux_udp::OriginalUdpDescription &original, sched::ProcessId pid, sched::ThreadId tid)
    {
        if (!access.live() || !admission.issuer_ || admission.issuer_ != access.capture_.owner_ ||
            admission.identity_ != access.capture_.identity_ || admission.identity_.pid != pid || admission.identity_.tid != tid ||
            !exact_original(original, admission.issuer_, admission.description_, admission.endpoint_) ||
            admission.issuer_->resolve_linux_udp_immediate(access.capture_)->scheduler != &original.scheduler_)
            return Status::denied("Linux UDP receive admission no longer owns its exact immediate capture");
        return {};
    }
    [[nodiscard]] static Result<Transfer> prepare(Access &access, const Admission &admission,
        const linux_udp::OriginalUdpDescription &original, memory::ObjectHandle object, const Owner &owner)
    {
        if (!access.live() || !admission.issuer_ || admission.issuer_ != access.capture_.owner_ ||
            admission.identity_ != access.capture_.identity_ || !object.pool || !object.generation ||
            owner.pid != admission.identity_.pid || owner.tid != admission.identity_.tid ||
            owner.phase != LinuxUdpReceivePhase::object_staged || owner.capture || owner.binding ||
            owner.retained_description != sched::invalid_description || owner.ticket.generation ||
            !exact_original(original, admission.issuer_, admission.description_, admission.endpoint_) ||
            admission.issuer_->resolve_linux_udp_immediate(access.capture_)->scheduler != &original.scheduler_)
            return Status::denied("Linux UDP receive transfer has no exact staged admission");
        return Transfer(admission, object);
    }
    [[nodiscard]] static Status commit(Transfer &transfer, Access &access,
        linux_udp::OriginalUdpDescription &original, Admission &admission, Owner &owner)
    {
        // Every check precedes mutation. No provider, scheduler arm or callback
        // occurs between consuming the old guard and installing the new owner.
        if (!transfer.issuer_ || transfer.issuer_ != admission.issuer_ ||
            transfer.identity_ != admission.identity_ || transfer.operation_ != admission.operation_ ||
            transfer.description_ != admission.description_ || transfer.endpoint_ != admission.endpoint_)
            return Status::denied("Linux UDP receive prepared transfer was changed");
        auto prepared = prepare(access, admission, original, transfer.object_, owner);
        if (!prepared) return prepared.status();
        auto *record = transfer.issuer_->owned(*original.scheduler_.find_thread(owner.pid, owner.tid));
        typename Origins::LinuxUdpReceiveBinding staged{transfer.object_, 0, {}, transfer.description_,
                                                       transfer.endpoint_, transfer.operation_};
        auto capture = typename Origins::LinuxUdpReceiveCapture(*transfer.issuer_, transfer.identity_);
        owner.operation = transfer.operation_;
        owner.endpoint = transfer.endpoint_;
        owner.retained_description = std::exchange(original.description_, sched::invalid_description);
        access.capture_.owner_ = nullptr;
        access.capture_.operation_ = LinuxUdpImmediateOperation::none;
        access.capture_.syscall_ = 0;
        record->kind = Origins::Kind::linux_udp_receive;
        record->linux_udp_receive_binding = staged;
        owner.capture.emplace(std::move(capture));
        owner.binding = staged;
        owner.phase = LinuxUdpReceivePhase::captured_staged;
        admission.issuer_ = nullptr;
        transfer.issuer_ = nullptr;
        return {};
    }
  private:
    static bool exact_original(const linux_udp::OriginalUdpDescription &original,
        const Origins *issuer, u16 description, u32 endpoint)
    {
        if (!issuer || description == sched::invalid_description || original.description_ != description) return false;
        return Origins::pure_linux_udp(original.scheduler_.open_description(description), endpoint);
    }
    [[nodiscard]] static Result<LinuxUdpReceivePreparation<Selected>> admit(Kernel &kernel, sched::ProcessControlBlock &process,
        sched::ThreadControlBlock &thread, Access &access, linux_udp::OriginalUdpDescription &original,
        linux_udp::ReceiveScratch &scratch)
    {
        if (!access.live() || kernel.scheduler().find(process.pid()) != &process ||
            kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
            kernel.scheduler().current_thread() != &thread || process.pid() != thread.owner ||
            &original.scheduler_ != &kernel.scheduler())
            return Status::denied("Linux UDP receive admission has no concrete current capture");
        using Preparation = LinuxUdpReceivePreparation<Selected>;
        auto immediate = [](i64 value) { return Preparation{{}, value}; };
        const auto decoded = linux_udp::decode_captured<Selected>(thread.native_frame, access);
        if (!decoded)
        {
            if (!access.live() || decoded.status().code() == StatusCode::denied) return decoded.status();
            return immediate(decoded.status().code() == StatusCode::not_found
                ? linux_udp_receive_errno<Selected>(abi::linux_user::UdpErrno::e_nosys)
                : linux_udp_receive_status<Selected>(decoded.status()));
        }
        return admit_decoded(kernel, process, thread, access, original, scratch, decoded.value());
    }
    // Only concrete OperationScratch decodes and selects this path. Copied
    // request values alone never grant receive admission or a capture transfer.
    [[nodiscard]] static Result<LinuxUdpReceivePreparation<Selected>> admit_decoded(
        Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
        Access &access, linux_udp::OriginalUdpDescription &original, linux_udp::ReceiveScratch &scratch,
        const abi::linux_user::UdpRequest &request)
    {
        if (!access.live() || kernel.scheduler().find(process.pid()) != &process ||
            kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
            kernel.scheduler().current_thread() != &thread || process.pid() != thread.owner ||
            &original.scheduler_ != &kernel.scheduler())
            return Status::denied("Linux UDP receive decoded admission has no concrete current capture");
        using Preparation = LinuxUdpReceivePreparation<Selected>;
        auto immediate = [](i64 value) { return Preparation{{}, value}; };
        using C = abi::linux_user::UdpConstants<Selected>;
        constexpr u64 mask = abi::linux_user::Codec<Selected>::word_mask;
        if (request.call != abi::linux_user::UdpCall::recvfrom)
            return immediate(linux_udp_receive_errno<Selected>(abi::linux_user::UdpErrno::e_opnotsupp));
        if (request.payload > mask || request.count > mask || request.payload > ~uptr{0} ||
            request.count > ~usize{0} || request.count > mask - request.payload)
            return immediate(linux_udp_receive_errno<Selected>(abi::linux_user::UdpErrno::e_fault));
        if (auto status = access.range(request.payload, request.count, true); !status)
            return access.live() ? Result<Preparation>{immediate(linux_udp_receive_status<Selected>(status))} : Result<Preparation>{status};
        const auto selected = original.acquire(process, request.fd);
        if (!selected) return immediate(linux_udp_receive_status<Selected>(selected.status()));
        if (selected.value().kind != linux_udp::DescriptionKind::retained_udp)
            return immediate(linux_udp_receive_errno<Selected>(selected.value().kind == linux_udp::DescriptionKind::bad_fd
                ? abi::linux_user::UdpErrno::e_badf : abi::linux_user::UdpErrno::e_notsock));
        // Preserve the immediate receive path's payload, OFD, flags order.
        if (request.flags & ~C::receive_flags_mask())
            return immediate(linux_udp_receive_errno<Selected>(abi::linux_user::UdpErrno::e_opnotsupp));
        if (!exact_original(original, access.capture_.owner_, original.description_, selected.value().endpoint))
            return Status::denied("Linux UDP receive original reference is not a readable pure UDP owner");
        const auto received = scratch.receive<Selected>(kernel.native_udp(), selected.value().endpoint, access,
            {.payload=request.payload, .count=request.count, .source=request.address,
             .source_length=request.source_length, .flags=request.flags});
        if (!access.live()) return Status::denied("Linux UDP receive admission lost capture after copy");
        if (received || received.status().code() != StatusCode::would_block)
            return immediate(received ? static_cast<i64>(received.value()) : linux_udp_receive_status<Selected>(received.status()));
        if (!kernel.native_udp_executor().status())
            return immediate(linux_udp_receive_errno<Selected>(abi::linux_user::UdpErrno::e_io));
        if ((selected.value().flags & posix::o_NONBLOCK) || (request.flags & *C::message(abi::linux_user::UdpMessage::dontwait)))
            return immediate(linux_udp_receive_errno<Selected>(abi::linux_user::UdpErrno::e_again));
        if (!exact_original(original, access.capture_.owner_, original.description_, selected.value().endpoint))
            return Status::denied("Linux UDP receive admission original reference changed");
        Preparation prepared;
        prepared.admission.emplace(Admission(access.capture_.owner_, access.capture_.identity_, request,
                                            original.description_, selected.value().endpoint));
        return prepared;
    }
};
} // namespace ok::user::native
