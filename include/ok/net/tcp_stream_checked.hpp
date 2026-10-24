#pragma once
#include <ok/net/native_tcp_stream.hpp>
#include <ok/sched/scheduler.hpp>
#include <ok/abi/linux_udp_call.hpp>
#include <ok/net/tcp_prepared_accept.hpp>

namespace ok::net
{
template <class Copy>
concept TcpUserCopy = requires(Copy &copy, u64 address, usize length,
                              std::span<std::byte> output, wire::Bytes input)
{
    { copy.preflight(address, length, true) } -> std::same_as<Status>;
    { copy.read(address, output) } -> std::same_as<Status>;
    { copy.write(address, input) } -> std::same_as<Status>;
};

// Shared native/Linux operations over canonical scheduler objects. The trusted
// user-copy implementation cannot call back into endpoint/descriptor mutation.
// Scratch belongs in static/resource storage. No numeric trap dispatch is added.
class TcpStreamOperations final
{
  public:
    static constexpr u32 nonblock = 0x0800;
    TcpStreamOperations(sched::SchedulerState &scheduler, NativeTcpStreamService &service)
        : scheduler_{scheduler}, service_{service} {}
    TcpStreamOperations(const TcpStreamOperations &) = delete;
    TcpStreamOperations &operator=(const TcpStreamOperations &) = delete;
    TcpStreamOperations(TcpStreamOperations &&) = delete;
    TcpStreamOperations &operator=(TcpStreamOperations &&) = delete;
    [[nodiscard]] Status status() const { return failure_; }
    Result<i32> open(u64 pid, bool nonblocking = false, bool close_on_exec = false)
    {
        if (mutating_) return Status::busy("TCP operation is not reentrant");
        Mutation mutation{mutating_};
        auto *process = canonical(pid); if (!process) return Status::denied("TCP process/service is not canonical");
        auto fd = vacant(*process); if (!fd) return fd.status();
        auto id = scheduler_.allocate_description({}); if (!id) return id.status();
        auto stream = service_.open();
        if (!stream) { *scheduler_.open_description(id.value()) = {}; return stream.status(); }
        return publish(*process, fd.value(), id.value(), stream.value(), nonblocking, close_on_exec);
    }
    template <class Action>
    auto with_stream(u64 pid, i32 fd, Action &&action) -> decltype(action(TcpStreamHandle{}))
    {
        using Return = decltype(action(TcpStreamHandle{}));
        if (mutating_) return Return{Status::busy("TCP operation is not reentrant")};
        Mutation mutation{mutating_};
        auto *process = canonical(pid); if (!process) return Return{Status::denied("TCP process/service is not canonical")};
        const auto *slot = process->fd_table().entry(fd);
        const auto *open = slot && slot->used ? scheduler_.open_description(slot->description) : nullptr;
        if (!open || !open->references || !open->native_tcp_handle || open->native_shared_file || !service_.valid(open->native_tcp_handle))
            return Return{Status::bad_descriptor("TCP operation descriptor is unavailable")};
        const auto id = slot->description; const auto handle = open->native_tcp_handle;
        if (auto status = scheduler_.retain_description(id); !status) return Return{status};
        auto result = action(handle);
        const auto release = scheduler_.release_description(id);
        if (!release && failure_) failure_ = release;
        return !release && result ? Return{release} : result;
    }
    Status bind(u64 pid, i32 fd, tcp::Endpoint local)
    { return with_stream(pid, fd, [&](TcpStreamHandle handle) { return service_.bind(handle, local); }); }
    Status listen(u64 pid, i32 fd, usize backlog)
    { return with_stream(pid, fd, [&](TcpStreamHandle handle) { return service_.listen(handle, backlog); }); }
    Status connect(u64 pid, i32 fd, tcp::Endpoint peer, u32 iss, u64 now)
    { return with_stream(pid, fd, [&](TcpStreamHandle handle) { return service_.connect(handle, peer, iss, now); }); }
    Status shutdown_write(u64 pid, i32 fd)
    { return with_stream(pid, fd, [&](TcpStreamHandle handle) { return service_.shutdown_write(handle); }); }
    Result<i32> accept(u64 pid, i32 listener, bool nonblocking = false, bool close_on_exec = false)
    {
        return with_stream(pid, listener, [&](TcpStreamHandle handle) -> Result<i32> {
            auto *process = canonical(pid);
            auto fd = vacant(*process); if (!fd) return fd.status();
            auto id = scheduler_.allocate_description({}); if (!id) return id.status();
            auto accepted = service_.accept(handle);
            if (!accepted) { *scheduler_.open_description(id.value()) = {}; return accepted.status(); }
            return publish(*process, fd.value(), id.value(), accepted.value(), nonblocking, close_on_exec);
        });
    }
    Result<PreparedTcpAccept> prepare_accept(u64 pid, i32 listener, bool nonblocking = false, bool close_on_exec = false)
    {
        auto operation = begin_accept(pid, listener, nonblocking, close_on_exec);
        if (!operation) return operation.status();
        if (auto status = operation.value().prepare_child(); !status) return status;
        return std::move(operation.value());
    }
    Result<PreparedTcpAccept> begin_accept(u64 pid, i32 listener, bool nonblocking = false, bool close_on_exec = false)
    {
        if (mutating_) return Status::busy("TCP prepared operation is not reentrant");
        mutating_ = true;
        struct Admission
        { bool &active; bool armed{true}; ~Admission() { if (armed) active = false; } } admission{mutating_};
        const auto *process = canonical(pid); if (!process) return Status::denied("TCP process/service is not canonical");
        const auto *slot = process->fd_table().entry(listener);
        const auto *open = slot && slot->used ? scheduler_.open_description(slot->description) : nullptr;
        if (!open || !slot->installation || !open->references || open->reservation || !open->native_tcp_handle ||
            !service_.valid(open->native_tcp_handle) || open->node || open->native_udp_handle || open->native_shared_file ||
            open->pipe != sched::invalid_description || open->unix_write_pipe != sched::invalid_description ||
            open->console || open->input_console)
            return Status::bad_descriptor("TCP prepared listener is not canonical");
        const auto id = slot->description; const auto installation = slot->installation;
        const auto handle = open->native_tcp_handle;
        auto state = service_.snapshot(handle);
        if (!state || !state.value().endpoint || state.value().endpoint->kind != tcp::HandleKind::listener)
            return Status::invalid_argument("TCP accept requires a listening stream");
        if (auto status = scheduler_.retain_description(id); !status) return status;
        auto retire_original = [&] {
            mutating_ = false; admission.armed = false;
            const auto status = scheduler_.release_description(id);
            if (!status && failure_) failure_ = status;
        };
        auto descriptor = scheduler_.prepare_descriptor(pid);
        if (!descriptor) { retire_original(); return descriptor.status(); }
        admission.armed = false;
        return PreparedTcpAccept{scheduler_, service_, std::move(descriptor.value()), TcpAcceptReservation{.listener = handle}, pid, listener,
            id, installation, nonblocking, close_on_exec, mutating_, failure_};
    }
    template <TcpUserCopy Copy>
    Result<usize> read(u64 pid, i32 fd, u64 address, usize count, Copy &copy)
    {
        return with_stream(pid, fd, [&](TcpStreamHandle handle) -> Result<usize> {
            if (count > (~usize{0} >> 1)) return Status::invalid_argument("TCP read count exceeds ssize range");
            const usize limit = count < scratch_.size() ? count : scratch_.size();
            auto preview = service_.peek(handle, std::span{scratch_}.first(limit)); if (!preview) return preview.status();
            const usize actual = preview.value();
            if (actual)
            {
                if (auto status = interval(address, actual); !status) return status;
                if (auto status = copy.preflight(address, actual, true); !status) return status;
                if (auto status = copy.write(address, wire::Bytes{scratch_.data(), actual}); !status) return status;
            }
            auto consumed = service_.read(handle, std::span{scratch_}.first(actual));
            if (!consumed || consumed.value() != actual)
            { failure_ = Status::fault("TCP receive changed during checked copy"); return failure_; }
            return actual;
        });
    }
    template <TcpUserCopy Copy>
    Result<usize> write(u64 pid, i32 fd, u64 address, usize count, Copy &copy)
    {
        return with_stream(pid, fd, [&](TcpStreamHandle handle) -> Result<usize> {
            if (count > (~usize{0} >> 1)) return Status::invalid_argument("TCP write count exceeds ssize range");
            const usize actual = count < scratch_.size() ? count : scratch_.size();
            if (actual)
            {
                if (auto status = interval(address, actual); !status) return status;
                if (auto status = copy.preflight(address, actual, false); !status) return status;
                if (auto status = copy.read(address, std::span{scratch_}.first(actual)); !status) return status;
            }
            return service_.write(handle, wire::Bytes{scratch_.data(), actual});
        });
    }
    [[nodiscard]] NativeTcpStreamService &service() { return service_; }
    static Status interval(u64 address, usize count)
    { return count && (address == 0 || address > ~u64{0} - (count - 1))
        ? Status::fault("TCP copied user interval wraps or is null") : Status::success(); }
  private:
    struct Mutation { bool &active; explicit Mutation(bool &value) : active{value} { active = true; } ~Mutation() { active = false; } };
    sched::ProcessControlBlock *canonical(u64 pid)
    {
        auto *process = scheduler_.native_tcp_service() == &service_ ? scheduler_.find(pid) : nullptr;
        return process && process->state() != sched::ProcessState::zombie && process->state() != sched::ProcessState::exited ? process : nullptr;
    }
    static Result<i32> vacant(const sched::ProcessControlBlock &process)
    {
        for (i32 fd = 3; fd < static_cast<i32>(sched::max_process_fds); ++fd)
            if (!process.fd_table().contains(fd)) return fd;
        return Status::process_file_limit("TCP process descriptor table is full");
    }
    Result<i32> publish(sched::ProcessControlBlock &process, i32 fd, u16 id,
                        TcpStreamHandle stream, bool nonblocking, bool close_on_exec)
    {
        auto *reserved = scheduler_.open_description(id);
        *reserved = sched::OpenFileDescription{.used = true, .readable = true, .writable = true,
            .flags = 2u | (nonblocking ? nonblock : 0u), .native_tcp_handle = stream};
        const auto status = scheduler_.install_descriptor(process.resources(), fd, id, close_on_exec);
        if (!status)
        {
            *reserved = {}; // Consume the unpublished edge before service retirement.
            const auto retired = service_.close(stream); if (!retired && failure_) failure_ = retired;
            return status;
        }
        return fd;
    }
    sched::SchedulerState &scheduler_; NativeTcpStreamService &service_;
    std::array<std::byte, 1024> scratch_{}; bool mutating_{}; Status failure_{};
};

template <abi::linux_user::Profile Selected>
    requires abi::linux_user::SupportedProfile<Selected>
class LinuxTcpStreamAdapter final
{
    using Constants = abi::linux_user::UdpConstants<Selected>;
    using Wire = abi::linux_user::UdpWire<Selected>;
  public:
    explicit LinuxTcpStreamAdapter(TcpStreamOperations &operations) : operations_{operations} {}
    Result<i32> socket(u64 pid, i32 domain, u32 type, i32 protocol)
    {
        if (domain != 2 || (protocol != 0 && protocol != 6) ||
            (type & Constants::sock_type_mask) != Constants::sock_stream ||
            (type & ~(Constants::sock_type_mask | Constants::sock_nonblock | Constants::sock_cloexec)))
            return Status::unsupported("Linux TCP socket parameters are outside the stream profile");
        return operations_.open(pid, type & Constants::sock_nonblock, type & Constants::sock_cloexec);
    }
    template <TcpUserCopy Copy>
    Status bind(u64 pid, i32 fd, u64 address, i32 length, Copy &copy)
    { return address_operation(pid, fd, address, length, copy,
        [&](TcpStreamHandle handle, tcp::Endpoint local) { return operations_.service().bind(handle, local); }); }
    template <TcpUserCopy Copy>
    Status connect(u64 pid, i32 fd, u64 address, i32 length, Copy &copy, u32 iss, u64 now)
    { return address_operation(pid, fd, address, length, copy,
        [&](TcpStreamHandle handle, tcp::Endpoint peer) { return operations_.service().connect(handle, peer, iss, now); }); }
    template <TcpUserCopy Copy>
    Result<usize> read(u64 pid, i32 fd, u64 address, u64 count, Copy &copy)
    {
        if (auto status = target_interval(address, count); !status) return status;
        return operations_.read(pid, fd, address, static_cast<usize>(count), copy);
    }
    template <TcpUserCopy Copy>
    Result<usize> write(u64 pid, i32 fd, u64 address, u64 count, Copy &copy)
    {
        if (auto status = target_interval(address, count); !status) return status;
        return operations_.write(pid, fd, address, static_cast<usize>(count), copy);
    }
    [[nodiscard]] static std::optional<u32> error_number(StatusCode code)
    {
        using Error = abi::linux_user::UdpErrno;
        switch (code)
        {
        case StatusCode::ok: return 0;
        case StatusCode::invalid_argument: return Constants::errno_number(Error::e_inval);
        case StatusCode::no_memory: return Constants::errno_number(Error::e_nomem);
        case StatusCode::already_exists: return Constants::errno_number(Error::e_addrinuse);
        case StatusCode::denied: case StatusCode::permission: return Constants::errno_number(Error::e_acces);
        case StatusCode::unsupported: return Constants::errno_number(Error::e_opnotsupp);
        case StatusCode::overflow: return Constants::errno_number(Error::e_overflow);
        case StatusCode::would_block: return Constants::errno_number(Error::e_again);
        case StatusCode::fault: return Constants::errno_number(Error::e_fault);
        case StatusCode::bad_descriptor: return Constants::errno_number(Error::e_badf);
        case StatusCode::io_error: return Constants::errno_number(Error::e_io);
        case StatusCode::process_file_limit: return Constants::errno_number(Error::e_mfile);
        case StatusCode::system_file_limit: return Constants::errno_number(Error::e_nfile);
        default: return std::nullopt;
        }
    }
    // Returns encode only explicitly sourced target errors. Numeric trap routing,
    // EINPROGRESS/SO_ERROR and signal restart publication are separate work.
    [[nodiscard]] static Status encode_result(abi::linux_user::RegisterFile &frame,
                                               Result<usize> result)
    {
        const auto number = result ? std::optional<u32>{0} : error_number(result.status().code());
        if (!number) return Status::unsupported("TCP status has no reviewed target errno mapping");
        return abi::linux_user::Codec<Selected>::encode_return(frame,
            {result ? static_cast<u64>(result.value()) : 0, *number});
    }
  private:
    static Status target_interval(u64 address, u64 count)
    {
        constexpr u64 mask = abi::linux_user::Codec<Selected>::word_mask;
        if (count > (mask >> 1) || count > ~usize{0}) return Status::invalid_argument("Linux TCP count exceeds target ssize range");
        const u64 actual = count < 1024 ? count : 1024;
        if (address > mask || (actual && address > mask - (actual - 1)))
            return Status::fault("Linux TCP payload interval exceeds target word");
        return {};
    }
    template <TcpUserCopy Copy, class Action>
    Status address_operation(u64 pid, i32 fd, u64 address, i32 length, Copy &copy, Action &&action)
    {
        return operations_.with_stream(pid, fd, [&](TcpStreamHandle handle) -> Status {
            if (length < 16 || length > 128) return Status::invalid_argument("Linux TCP sockaddr length is invalid");
            const usize count = static_cast<usize>(length);
            if (address > abi::linux_user::Codec<Selected>::word_mask - (count - 1))
                return Status::fault("Linux TCP address interval exceeds target word");
            if (auto status = TcpStreamOperations::interval(address, count); !status) return status;
            if (auto status = copy.preflight(address, count, false); !status) return status;
            std::array<std::byte, 128> bytes{};
            if (auto status = copy.read(address, std::span{bytes}.first(count)); !status) return status;
            std::array<u8, 128> copied{};
            for (usize i = 0; i < count; ++i) copied[i] = std::to_integer<u8>(bytes[i]);
            auto decoded = Wire::decode_address(std::span{copied}.first(count)); if (!decoded) return decoded.status();
            if (decoded.value().family != 2) return Status::unsupported("Linux TCP address family is not IPv4");
            return action(handle, tcp::Endpoint{decoded.value().address, decoded.value().port});
        });
    }
    TcpStreamOperations &operations_;
};
}
