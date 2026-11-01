#pragma once

#include "linux_udp_original_ofd.hpp"
#include "linux_udp_receive_authority.hpp"
#include "linux_udp_receive_order.hpp"
#include "ok/abi/linux_udp_call.hpp"
#include "ok/core/kernel.hpp"

namespace ok::user::native { inline namespace runtime_detail { class LinuxUdpOperationRuntimeAuthority; } }

namespace ok::user::native::linux_udp
{
template <class Access>
concept OperationAccess = ReceiveAccess<Access> && ImmediateReset<Access> &&
    requires(Access &access, uptr pointer, usize size, bool write) {
        { access.range(pointer, size, write) } -> std::same_as<Status>;
    };

// Static composition shares operation mechanics; runtime polymorphism remains
// at the actual extension device-service boundary.
class KernelOperationBackend final
{
  public:
    explicit KernelOperationBackend(Kernel &kernel) : kernel_{kernel} {}
    sched::SchedulerState &scheduler() { return kernel_.scheduler(); }
    net::NativeUdpService &service() { return kernel_.native_udp(); }
    Status executor_status() const { return kernel_.native_udp_executor().status(); }
    Result<net::NativeUdpLink> link()
    {
        auto *extension = kernel_.extension();
        return extension ? extension->network_link(kernel_)
                         : Result<net::NativeUdpLink>{Status::not_initialized("Linux UDP link is unavailable")};
    }
    bool current(const sched::ProcessControlBlock &process)
    {
        return kernel_.scheduler().find(process.pid()) == &process &&
               kernel_.posix().getpid() == process.pid();
    }
    Result<i32> install(sched::ProcessControlBlock &process, u32 endpoint, bool cloexec, bool nonblock)
    {
        if (!current(process)) return Status::denied("Linux UDP descriptor owner changed");
        return kernel_.posix().install_native_udp(endpoint, cloexec, nonblock);
    }
    Status submit(std::span<const std::byte> frame)
    {
        auto *extension = kernel_.extension();
        return extension ? extension->network_try_transmit(kernel_, frame)
                         : Status::not_initialized("Linux UDP transmitter is unavailable");
    }
  private:
    Kernel &kernel_;
};

template <class Backend>
concept OperationBackend = requires(Backend &backend, sched::ProcessControlBlock &process,
                                    u32 endpoint, bool cloexec, bool nonblock, std::span<const std::byte> frame) {
    { backend.scheduler() } -> std::same_as<sched::SchedulerState &>;
    { backend.service() } -> std::same_as<net::NativeUdpService &>;
    { backend.executor_status() } -> std::same_as<Status>;
    { backend.link() } -> std::same_as<Result<net::NativeUdpLink>>;
    { backend.current(process) } -> std::same_as<bool>;
    { backend.install(process, endpoint, cloexec, nonblock) } -> std::same_as<Result<i32>>;
    { backend.submit(frame) } -> std::same_as<Status>;
};

class OperationScratch final
{
  public:
    OperationScratch() = default;
    OperationScratch(const OperationScratch &) = delete;
    OperationScratch &operator=(const OperationScratch &) = delete;
    OperationScratch(OperationScratch &&) = delete;
    OperationScratch &operator=(OperationScratch &&) = delete;

    // Concrete-decoding convenience; the generic hosted backend seam cannot
    // mint this move-only capability or call the already-decoded coordinator.
    template <abi::linux_user::Profile Target>
    [[nodiscard]] Result<LinuxUdpReceivePreparation<Target>> prepare_blocking_receive(
        Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread,
        LinuxUdpImmediateAccess<Target> &access, OriginalUdpDescription &original)
    {
        if (busy_) return Status::busy("recursive Linux UDP receive admission scratch entry");
        ScratchScope scratch{*this};
        return LinuxUdpReceiveCoordinator<Target>::admit(kernel, process, thread, access, original, receive_);
    }

  private:
    friend LinuxUdpOperationRuntimeAuthority;
    // A concrete entry has one decoder, one original OFD guard and one
    // immediate publisher. Neither an arbitrary backend nor copied requests
    // can select this capability handoff.
    template <abi::linux_user::Profile Target, class Begin, class Publish>
        requires requires(Begin &begin, Publish &publish, LinuxUdpImmediateAccess<Target> &access,
                          OriginalUdpDescription &original, LinuxUdpReceiveAdmission<Target> &&admission, i64 value) {
            { begin(access, original, std::move(admission)) } -> std::same_as<Result<LinuxUdpReceiveBegin>>;
            { publish(value) } -> std::same_as<Status>;
        }
    [[nodiscard]] Status dispatch(Kernel &kernel, sched::ProcessControlBlock &process,
        sched::ThreadControlBlock &thread, LinuxUdpImmediateAccess<Target> &access,
        Begin &begin, Publish &publish, Status &release_failure)
    {
        auto failed = [&](Status status) -> Status {
            // The concrete sink is runtime_failure. Contain before local scope
            // destruction or final OFD release can cross a cleanup boundary.
            if (!status && release_failure) release_failure = status;
            return status;
        };
        if (busy_) return failed(Status::busy("recursive Linux UDP concrete dispatch scratch entry"));
        ScratchScope scratch{*this};
        KernelOperationBackend backend{kernel};
        OriginalUdpDescription original{kernel.scheduler(), release_failure};
        ImmediateLifetimeScope lifetime{access, original};
        if (!access.live() || !backend.current(process) ||
            kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
            kernel.scheduler().current_thread() != &thread || thread.owner != process.pid())
            return failed(Status::denied("Linux UDP concrete dispatch has no current captured thread"));
        auto immediate = [&](i64 value) -> Status {
            if (!access.live() || !backend.current(process) ||
                kernel.scheduler().find_thread(process.pid(), thread.tid) != &thread ||
                kernel.scheduler().current_thread() != &thread) return failed(lost());
            const auto published = publish(value);
            if (!published) static_cast<void>(failed(published));
            const auto released = lifetime.finish();
            return !published ? published : released;
        };
        const auto decoded = decode_captured<Target>(thread.native_frame, access);
        if (!decoded)
        {
            if (!access.live()) return failed(lost());
            switch (decoded.status().code())
            {
            case StatusCode::invalid_argument: return immediate(error<Target>(Error::e_inval));
            case StatusCode::fault: return immediate(error<Target>(Error::e_fault));
            case StatusCode::overflow: return immediate(error<Target>(Error::e_overflow));
            case StatusCode::unsupported: return immediate(error<Target>(Error::e_opnotsupp));
            case StatusCode::not_found: return immediate(error<Target>(Error::e_nosys));
            default: return failed(decoded.status());
            }
        }
        if (!access.live()) return failed(lost());
        const auto &request = decoded.value();
        if (request.call != abi::linux_user::UdpCall::recvfrom)
            return immediate(operate<Target>(backend, process, access, original, request, release_failure));
        auto prepared = LinuxUdpReceiveCoordinator<Target>::admit_decoded(
            kernel, process, thread, access, original, receive_, request);
        if (!prepared) return failed(prepared.status());
        auto &preparation = prepared.value();
        if (preparation.admission.has_value() == preparation.target_result.has_value())
            return failed(Status::fault("Linux UDP receive preparation has no unique result or admission"));
        if (preparation.target_result) return immediate(*preparation.target_result);
        const auto begun = begin(access, original, std::move(*preparation.admission));
        if (!begun) return failed(begun.status());
        const auto &outcome = begun.value();
        switch (outcome.kind)
        {
        case LinuxUdpReceiveBeginKind::immediate:
            if (!outcome.target_result || !access.live() || !original.held())
                return failed(Status::fault("Linux UDP receive immediate begin lost its caller lifetime"));
            return immediate(*outcome.target_result);
        case LinuxUdpReceiveBeginKind::deferred:
        case LinuxUdpReceiveBeginKind::owned_completed:
            if (outcome.target_result || access.live() || original.held())
                return failed(Status::fault("Linux UDP receive owned begin retained immediate authority"));
            return lifetime.finish();
        }
        return failed(Status::fault("Linux UDP receive begin kind is invalid"));
    }

  public:
    template <abi::linux_user::Profile Target, OperationAccess Access, class Decode, class Publish>
    [[nodiscard]] Status run(Kernel &kernel, sched::ProcessControlBlock &process,
                             Access &access, Decode &decode, Publish &publish, Status &release_failure)
    {
        KernelOperationBackend backend{kernel};
        return run_backend<Target>(backend, process, access, decode, publish, release_failure);
    }

    // Explicit synthetic backend seam for hosted operation models. It does not
    // supply raw dispatcher or current-image authority by itself.
    template <abi::linux_user::Profile Target, OperationBackend Backend,
              OperationAccess Access, class Decode, class Publish>
        requires requires(Decode &decode, Publish &publish, i64 value) {
            { decode() } -> std::same_as<Result<abi::linux_user::UdpRequest>>;
            { publish(value) } -> std::same_as<Status>;
        }
    [[nodiscard]] Status run_backend(Backend &backend, sched::ProcessControlBlock &process,
                                     Access &access, Decode &decode, Publish &publish,
                                     Status &release_failure)
    {
        // A recursive caller must not reset the access used by the outer call.
        if (busy_) return Status::busy("recursive Linux UDP operation scratch entry");
        ScratchScope scratch{*this};
        OriginalUdpDescription original{backend.scheduler(), release_failure};
        ImmediateLifetimeScope lifetime{access, original};
        if (!access.live() || !backend.current(process))
            return Status::denied("Linux UDP operation has no current image owner");
        const auto request = decode();
        i64 result{};
        if (!request)
        {
            // The decoder's typed failures are distinct from operation status.
            switch (request.status().code())
            {
            case StatusCode::invalid_argument: result = error<Target>(Error::e_inval); break;
            case StatusCode::fault: result = error<Target>(Error::e_fault); break;
            case StatusCode::overflow: result = error<Target>(Error::e_overflow); break;
            case StatusCode::unsupported: result = error<Target>(Error::e_opnotsupp); break;
            case StatusCode::not_found: result = error<Target>(Error::e_nosys); break;
            default: return request.status();
            }
        }
        else
        {
            if (!access.live()) return lost();
            result = operate<Target>(backend, process, access, original, request.value(), release_failure);
        }
        if (!access.live() || !backend.current(process)) return lost();
        // Root publisher changes the captured frame synchronously. Its success
        // is never cached as permission for publication after lifetime.finish.
        const auto published = publish(result);
        const auto released = lifetime.finish();
        return !published ? published : released;
    }

  private:
    template <abi::linux_user::Profile Target> using Constants = abi::linux_user::UdpConstants<Target>;
    using Error = abi::linux_user::UdpErrno;
    template <abi::linux_user::Profile Target> static i64 error(Error value)
    {
        return -static_cast<i64>(*Constants<Target>::errno_number(value));
    }
    template <abi::linux_user::Profile Target> static i64 status_error(Status status)
    {
        switch (status.code())
        {
        case StatusCode::fault: return error<Target>(Error::e_fault);
        case StatusCode::invalid_argument: return error<Target>(Error::e_inval);
        case StatusCode::no_memory: return error<Target>(Error::e_nomem);
        case StatusCode::process_file_limit: return error<Target>(Error::e_mfile);
        case StatusCode::system_file_limit: return error<Target>(Error::e_nfile);
        case StatusCode::would_block: return error<Target>(Error::e_again);
        case StatusCode::unsupported: return error<Target>(Error::e_opnotsupp);
        case StatusCode::not_initialized: return error<Target>(Error::e_netdown);
        case StatusCode::message_size: return error<Target>(Error::e_msgsize);
        case StatusCode::overflow: return error<Target>(Error::e_overflow);
        case StatusCode::bad_descriptor: return error<Target>(Error::e_badf);
        default: return error<Target>(Error::e_io);
        }
    }
    static Status lost() { return Status::denied("Linux UDP operation image authority expired"); }
    static void record(Status status, Status &failure) { if (!status && failure) failure = status; }
    static bool any(net::wire::Ipv4 address) { return address == net::wire::Ipv4{}; }
    // A stale handle keeps the IPv4 path, which reports it at its first use.
    template <OperationBackend Backend> static bool ipv6(Backend &backend, u32 endpoint)
    {
        const auto family = backend.service().family(endpoint);
        return family && family.value() == net::Family::ipv6;
    }

    template <abi::linux_user::Profile Target, OperationAccess Access>
    static Status payload_range(Access &access, const abi::linux_user::UdpRequest &request)
    {
        constexpr u64 mask = abi::linux_user::Codec<Target>::word_mask;
        if (request.payload > mask || request.count > mask || request.payload > ~uptr{0} ||
            request.count > ~usize{0} || request.count > mask - request.payload)
            return Status::fault("Linux UDP payload interval wraps the target word");
        return access.range(static_cast<uptr>(request.payload), static_cast<usize>(request.count),
                            request.call == abi::linux_user::UdpCall::recvfrom);
    }

    template <OperationAccess Access>
    Status copy_address(Access &access, u64 pointer, i32 length)
    {
        if (length < 0 || static_cast<u32>(length) > address_.size())
            return Status::invalid_argument("Linux UDP input address length is outside 0..128");
        if (!access.live()) return lost();
        const usize count = static_cast<u32>(length);
        if (count)
            if (auto status = access.read(pointer,
                    {reinterpret_cast<std::byte *>(address_.data()), count}); !status) return status;
        return access.live() ? Status{} : lost();
    }

    template <abi::linux_user::Profile Target, OperationAccess Access>
    Result<abi::linux_user::Ipv4SocketAddress> address(Access &access, u64 pointer, i32 length)
    {
        if (auto status = copy_address(access, pointer, length); !status) return status;
        return abi::linux_user::UdpWire<Target>::decode_address(
            {address_.data(), static_cast<usize>(length)});
    }

    template <abi::linux_user::Profile Target, OperationBackend Backend, OperationAccess Access>
    i64 operate(Backend &backend, sched::ProcessControlBlock &process, Access &access,
                OriginalUdpDescription &original, const abi::linux_user::UdpRequest &request,
                Status &release_failure)
    {
        namespace lu = abi::linux_user;
        if (request.call == lu::UdpCall::socket)
        {
            using C = Constants<Target>;
            if (request.domain != static_cast<i32>(C::af_inet) && request.domain != static_cast<i32>(C::af_inet6))
                return error<Target>(Error::e_afnosupport);
            const u32 type = static_cast<u32>(request.type);
            if (type & ~(C::sock_type_mask | C::sock_nonblock | C::sock_cloexec))
                return error<Target>(Error::e_inval);
            if ((type & C::sock_type_mask) != C::sock_dgram) return error<Target>(Error::e_socktnosupport);
            if (request.protocol && request.protocol != static_cast<i32>(C::protocol_udp))
                return error<Target>(Error::e_protonosupport);
            if (!access.live()) return error<Target>(Error::e_io);
            if (auto status = backend.executor_status(); !status) return error<Target>(Error::e_io);
            if (!backend.service().configured())
            {
                const auto link = backend.link();
                // Extension callbacks may revoke the captured image, including
                // on success. No service mutation follows a lost observation.
                if (!access.live() || !backend.current(process)) return error<Target>(Error::e_io);
                if (!link) return status_error<Target>(link.status());
                if (auto status = backend.service().configure(link.value()); !status)
                    return status_error<Target>(status);
            }
            const auto endpoint = backend.service().open(
                request.domain == static_cast<i32>(C::af_inet6) ? net::Family::ipv6 : net::Family::ipv4);
            if (!endpoint) return status_error<Target>(endpoint.status());
            if (!access.live())
            {
                record(backend.service().close(endpoint.value()), release_failure);
                return error<Target>(Error::e_io);
            }
            const auto installed = backend.install(process, endpoint.value(), (type & C::sock_cloexec) != 0,
                                                   (type & C::sock_nonblock) != 0);
            if (!installed)
            {
                record(backend.service().close(endpoint.value()), release_failure);
                return status_error<Target>(installed.status());
            }
            return installed.value();
        }
        if (request.call == lu::UdpCall::sendto || request.call == lu::UdpCall::recvfrom)
        {
            if (const auto status = payload_range<Target>(access, request); !status)
                return status_error<Target>(status);
            if (!access.live()) return error<Target>(Error::e_io);
        }
        if (request.call == lu::UdpCall::connect)
        {
            const auto candidate = original.acquire_connect_candidate(process, request.fd);
            if (!candidate) return status_error<Target>(candidate.status());
            if (candidate.value().kind == DescriptionKind::bad_fd) return error<Target>(Error::e_badf);
            // Linux copies a valid descriptor's address before the socket type check.
            if (auto status = copy_address(access, request.address, request.address_length); !status)
                return status_error<Target>(status);
            if (!access.live() || !backend.current(process)) return error<Target>(Error::e_io);
            if (candidate.value().kind == DescriptionKind::not_socket) return error<Target>(Error::e_notsock);
            const auto endpoint = candidate.value().endpoint;
            const auto family = lu::UdpWire<Target>::decode_family(
                {address_.data(), static_cast<usize>(request.address_length)});
            if (!family) return status_error<Target>(family.status());
            if (family.value() == Constants<Target>::af_unspec)
            {
                const auto disconnected = backend.service().disconnect(endpoint);
                return disconnected ? 0 : status_error<Target>(disconnected);
            }
            // Unsupported families and short records retain this reservation.
            const auto bound = backend.service().autobind(endpoint);
            if (!bound) return bound.code() == StatusCode::no_space
                ? error<Target>(Error::e_again) : status_error<Target>(bound);
            if (ipv6(backend, endpoint))
            {
                // __ip6_datagram_connect on a v6-only socket: AF_INET, then
                // the RFC 2133 length, then any other family.
                if (family.value() == Constants<Target>::af_inet) return error<Target>(Error::e_afnosupport);
                const auto peer = lu::UdpWire<Target>::decode_ipv6_address(
                    {address_.data(), static_cast<usize>(request.address_length)});
                if (!peer) return status_error<Target>(peer.status());
                if (peer.value().family != Constants<Target>::af_inet6) return error<Target>(Error::e_afnosupport);
                const net::IpAddress address{peer.value().address};
                if (net::wire::ipv4_mapped(peer.value().address) || !backend.service().reachable(address))
                    return error<Target>(Error::e_netunreach);
                const auto connected = backend.service().connect(endpoint, address, peer.value().port);
                return connected ? 0 : status_error<Target>(connected);
            }
            const auto peer = lu::UdpWire<Target>::decode_address(
                {address_.data(), static_cast<usize>(request.address_length)});
            if (!peer) return status_error<Target>(peer.status());
            if (peer.value().family != Constants<Target>::af_inet) return error<Target>(Error::e_afnosupport);
            if (!backend.service().reachable(peer.value().address)) return error<Target>(Error::e_netunreach);
            const auto connected = backend.service().connect(endpoint, peer.value().address, peer.value().port);
            return connected ? 0 : status_error<Target>(connected);
        }
        const auto selected = original.acquire(process, request.fd);
        if (!selected) return status_error<Target>(selected.status());
        if (selected.value().kind == DescriptionKind::bad_fd) return error<Target>(Error::e_badf);
        if (selected.value().kind == DescriptionKind::not_socket) return error<Target>(Error::e_notsock);
        const auto endpoint = selected.value().endpoint;
        if (!access.live()) return error<Target>(Error::e_io);
        if (request.call == lu::UdpCall::getsockname || request.call == lu::UdpCall::getpeername)
        {
            // Linux obtains the canonical name before reading user capacity.
            // The original retained OFD survives descriptor reuse in callbacks.
            // The snapshot address carries the socket family, bound or not.
            net::IpAddress address{};
            u16 port{};
            if (request.call == lu::UdpCall::getpeername)
            {
                const auto peer = backend.service().peer(endpoint);
                if (!peer) return status_error<Target>(peer.status());
                if (!peer.value().associated || !peer.value().port)
                    return -static_cast<i64>(Constants<Target>::not_connected_errno_number());
                address = peer.value().address;
                port = peer.value().port;
            }
            else
            {
                const auto binding = backend.service().binding(endpoint);
                if (!binding) return status_error<Target>(binding.status());
                address = binding.value().address;
                port = binding.value().port;
            }
            const auto name = lu::encode_socket_name<Target>(address, port);
            if (!name) return status_error<Target>(name.status());
            std::array<u8, 4> length{};
            if (auto status = access.read(request.source_length,
                    {reinterpret_cast<std::byte *>(length.data()), length.size()}); !status)
                return status_error<Target>(status);
            if (!access.live() || !backend.current(process)) return error<Target>(Error::e_io);
            const auto capacity = lu::UdpWire<Target>::decode_socklen(length);
            if (!capacity) return status_error<Target>(capacity.status());
            if (capacity.value() < 0) return error<Target>(Error::e_inval);
            const usize count = static_cast<u32>(capacity.value()) < name.value().size
                ? static_cast<u32>(capacity.value()) : name.value().size;
            if (count)
            {
                if (auto status = access.write(request.address,
                        {reinterpret_cast<const std::byte *>(name.value().bytes.data()), count}); !status)
                    return status_error<Target>(status);
                if (!access.live() || !backend.current(process)) return error<Target>(Error::e_io);
            }
            if (auto status = lu::UdpWire<Target>::encode_socklen(length, name.value().size); !status)
                return status_error<Target>(status);
            if (auto status = access.write(request.source_length,
                    {reinterpret_cast<const std::byte *>(length.data()), length.size()}); !status)
                return status_error<Target>(status);
            return access.live() && backend.current(process) ? 0 : error<Target>(Error::e_io);
        }
        if ((request.call == lu::UdpCall::sendto || request.call == lu::UdpCall::recvfrom) &&
            !(selected.value().flags & posix::o_NONBLOCK) &&
            !(request.flags & *Constants<Target>::message(lu::UdpMessage::dontwait)))
            return error<Target>(Error::e_opnotsupp);
        if (request.call == lu::UdpCall::bind)
        {
            if (ipv6(backend, endpoint)) return bind_ipv6<Target>(backend, access, request, endpoint);
            const auto local = address<Target>(access, request.address, request.address_length);
            if (!local) return status_error<Target>(local.status());
            const auto &value = local.value();
            if (value.family != Constants<Target>::af_inet &&
                !(value.family == Constants<Target>::af_unspec && any(value.address)))
                return error<Target>(Error::e_afnosupport);
            if (!any(value.address) && !backend.service().local_address(value.address))
                return error<Target>(Error::e_addrnotavail);
            if (value.port && value.port < 1024) return error<Target>(Error::e_acces);
            const auto state = backend.service().binding(endpoint);
            if (!state) return status_error<Target>(state.status());
            if (state.value().bound) return error<Target>(Error::e_inval);
            if (!access.live()) return error<Target>(Error::e_io);
            const auto bound = value.port
                ? backend.service().bind(endpoint, value.address, value.port)
                : backend.service().bind_ephemeral(endpoint, value.address);
            return bound ? 0 : bound.code() == StatusCode::already_exists || bound.code() == StatusCode::no_space
                ? error<Target>(Error::e_addrinuse) : status_error<Target>(bound);
        }
        if (request.call == lu::UdpCall::sendto)
        {
            // Linux copies a supplied sockaddr before inet_send_prepare autobinds.
            if (request.address)
                if (auto status = copy_address(access, request.address, request.address_length); !status)
                    return status_error<Target>(status);
            if (!access.live()) return error<Target>(Error::e_io);
            const auto binding = backend.service().binding(endpoint);
            if (!binding) return status_error<Target>(binding.status());
            if (!binding.value().bound)
            {
                const auto bound = backend.service().autobind(endpoint);
                if (!bound) return bound.code() == StatusCode::no_space
                    ? error<Target>(Error::e_again) : status_error<Target>(bound);
            }
            // The canonical port persists through these later protocol errors.
            if (request.count > 0xffffU) return error<Target>(Error::e_msgsize);
            if (request.flags & ~Constants<Target>::send_flags_mask()) return error<Target>(Error::e_opnotsupp);
            net::IpAddress destination{};
            u16 port{};
            if (ipv6(backend, endpoint))
            {
                const auto selected = ipv6_destination<Target>(backend, request, endpoint);
                if (selected.error) return selected.error;
                destination = selected.address;
                port = selected.port;
            }
            else if (lu::Ipv4SocketAddress value{}; request.address)
            {
                const auto peer = lu::UdpWire<Target>::decode_address(
                    {address_.data(), static_cast<usize>(request.address_length)});
                if (!peer) return status_error<Target>(peer.status());
                value = peer.value();
                if (value.family != Constants<Target>::af_inet && value.family != Constants<Target>::af_unspec)
                    return error<Target>(Error::e_afnosupport);
                if (!value.port) return error<Target>(Error::e_inval);
                if (!backend.service().reachable(value.address)) return error<Target>(Error::e_netunreach);
                destination = value.address;
                port = value.port;
            }
            else
            {
                const auto peer = backend.service().peer(endpoint);
                if (!peer) return status_error<Target>(peer.status());
                if (!peer.value().associated) return error<Target>(Error::e_destaddrreq);
                destination = peer.value().address;
                port = peer.value().port;
            }
            if (request.count > payload_.size()) return error<Target>(Error::e_msgsize);
            if (!access.live()) return error<Target>(Error::e_io);
            const usize count = static_cast<usize>(request.count);
            if (auto status = access.read(request.payload, {payload_.data(), count}); !status)
                return status_error<Target>(status);
            if (!access.live()) return error<Target>(Error::e_io);
            if (auto status = backend.executor_status(); !status) return error<Target>(Error::e_io);
            const auto built = backend.service().build_datagram(endpoint, destination, port,
                                                               {payload_.data(), count}, frame_);
            if (!built) return status_error<Target>(built.status());
            if (!access.live()) return error<Target>(Error::e_io);
            // Held for neighbour resolution: the interface transmits it later.
            if (!built.value()) return static_cast<i64>(count);
            const auto submitted = backend.submit({frame_.data(), built.value()});
            return submitted ? static_cast<i64>(count) : status_error<Target>(submitted);
        }
        if (request.call == lu::UdpCall::recvfrom)
        {
            if (request.flags & ~Constants<Target>::receive_flags_mask()) return error<Target>(Error::e_opnotsupp);
            const auto received = receive_.receive<Target>(backend.service(), endpoint, access,
                {.payload = request.payload, .count = request.count, .source = request.address,
                 .source_length = request.source_length, .flags = request.flags});
            if (received) return static_cast<i64>(received.value());
            if (received.status().code() == StatusCode::would_block && !backend.executor_status())
                return error<Target>(Error::e_io);
            return status_error<Target>(received.status());
        }
        return error<Target>(Error::e_nosys);
    }

    // inet6_bind_sk/__inet6_bind order for a v6-only socket; scope_id is
    // ignored because the kernel has one interface.
    template <abi::linux_user::Profile Target, OperationBackend Backend, OperationAccess Access>
    i64 bind_ipv6(Backend &backend, Access &access, const abi::linux_user::UdpRequest &request, u32 endpoint)
    {
        if (auto status = copy_address(access, request.address, request.address_length); !status)
            return status_error<Target>(status);
        const auto local = abi::linux_user::UdpWire<Target>::decode_ipv6_address(
            {address_.data(), static_cast<usize>(request.address_length)});
        if (!local) return status_error<Target>(local.status());
        if (local.value().family != Constants<Target>::af_inet6) return error<Target>(Error::e_afnosupport);
        const u16 port = local.value().port;
        if (port && port < 1024) return error<Target>(Error::e_acces);
        const auto state = backend.service().binding(endpoint);
        if (!state) return status_error<Target>(state.status());
        if (state.value().bound || net::wire::ipv4_mapped(local.value().address)) return error<Target>(Error::e_inval);
        const net::IpAddress address{local.value().address};
        if (!address.unspecified() && !backend.service().local_address(address))
            return error<Target>(Error::e_addrnotavail);
        if (!access.live()) return error<Target>(Error::e_io);
        const auto bound = port ? backend.service().bind(endpoint, address, port)
                                : backend.service().bind_ephemeral(endpoint, address);
        return bound ? 0 : bound.code() == StatusCode::already_exists || bound.code() == StatusCode::no_space
            ? error<Target>(Error::e_addrinuse) : status_error<Target>(bound);
    }

    struct Destination { net::IpAddress address{}; u16 port{}; i64 error{}; };
    // udpv6_sendmsg on a v6-only socket over the already copied sockaddr:
    // AF_UNSPEC selects the peer, AF_INET and mapped addresses are unreachable.
    template <abi::linux_user::Profile Target, OperationBackend Backend>
    Destination ipv6_destination(Backend &backend, const abi::linux_user::UdpRequest &request, u32 endpoint)
    {
        namespace lu = abi::linux_user;
        const std::span<const u8> copied{address_.data(), static_cast<usize>(request.address_length)};
        bool peer = !request.address;
        if (!peer)
        {
            const auto family = lu::UdpWire<Target>::decode_family(copied);
            if (!family) return {.error = status_error<Target>(family.status())};
            if (family.value() == Constants<Target>::af_inet) return {.error = error<Target>(Error::e_netunreach)};
            if (family.value() == Constants<Target>::af_unspec) peer = true;
            else if (family.value() != Constants<Target>::af_inet6) return {.error = error<Target>(Error::e_inval)};
        }
        if (peer)
        {
            const auto connected = backend.service().peer(endpoint);
            if (!connected) return {.error = status_error<Target>(connected.status())};
            if (!connected.value().associated) return {.error = error<Target>(Error::e_destaddrreq)};
            return {connected.value().address, connected.value().port, 0};
        }
        const auto value = lu::UdpWire<Target>::decode_ipv6_address(copied);
        if (!value) return {.error = status_error<Target>(value.status())};
        if (net::wire::ipv4_mapped(value.value().address)) return {.error = error<Target>(Error::e_netunreach)};
        if (!value.value().port) return {.error = error<Target>(Error::e_inval)};
        const net::IpAddress address{value.value().address};
        if (!backend.service().reachable(address)) return {.error = error<Target>(Error::e_netunreach)};
        return {address, value.value().port, 0};
    }

    class ScratchScope final
    {
      public:
        explicit ScratchScope(OperationScratch &scratch) : scratch_{scratch} { scratch_.busy_ = true; }
        ~ScratchScope() { scratch_.busy_ = false; }
        ScratchScope(const ScratchScope &) = delete;
        ScratchScope &operator=(const ScratchScope &) = delete;
      private:
        OperationScratch &scratch_;
    };
    bool busy_{};
    std::array<std::byte, net::NativeUdpService::datagram_capacity> payload_{};
    std::array<std::byte, net::NativeUdpService::frame_capacity> frame_{};
    std::array<u8, abi::linux_user::udp_sockaddr_input_limit> address_{};
    ReceiveScratch receive_;
};
} // namespace ok::user::native::linux_udp
