// Included inside the runtime's private namespace after target publication.
class TcpImmediateRuntime final
{
    using Access = TcpImmediateAccess<lu::configured_native_profile()>;
    using Call = lu::TcpCall;
    using Constants = lu::UdpConstants<lu::configured_native_profile()>;
    using AcceptCall = lu::TcpAcceptCall<lu::configured_native_profile()>;
    static constexpr usize profile = static_cast<usize>(lu::configured_native_profile());
    struct Request { Call operation{}; bool linux{}; std::array<u64, 6> arguments{}; };
  public:
    Status dispatch(Kernel &kernel, sched::ProcessControlBlock &process, sched::ThreadControlBlock &thread)
    {
        if (busy_) return Status::busy("TCP immediate runtime is not reentrant");
        struct Scope { bool &busy; explicit Scope(bool &value) : busy(value) { busy = true; } ~Scope() { busy = false; } } scope{busy_};
        auto admitted = Access::acquire(syscall_origins, process, thread);
        if (!admitted) return admitted.status();
        auto &access = admitted.value();
        // Only owned frame values and captured IDs survive copied callbacks.
        const auto saved_frame = thread.native_frame;
        const u64 pid = access.pid();
        const auto route = access.linux_personality()
            ? AcceptCall::route(lu::NativeBridge<lu::configured_native_profile()>::snapshot(saved_frame))
            : lu::TcpCallRoute{access.operation(), false, 0, 0};
        TcpOriginalDescription original{kernel.scheduler(), kernel.native_tcp(), runtime_failure};
        const auto operation = [&]() -> Status
        {
            if (!access.live()) return Status::denied("TCP dispatch lost captured image");
            if (route.operation != access.operation()) return Status::denied("TCP effective route differs from original capture");
            if (access.linux_personality())
            {
                using Codec = lu::Codec<lu::configured_native_profile()>;
                const auto registers = lu::NativeBridge<lu::configured_native_profile()>::snapshot(saved_frame);
                if (registers.integer[Codec::profile.number_register] != access.raw_syscall())
                    return Status::denied("TCP outer raw syscall differs from captured frame");
            }
            if (access.operation() == Call::accept || access.operation() == Call::accept4)
            {
                auto captured = TcpCapturedAccept<lu::configured_native_profile()>::capture(
                    kernel.scheduler(), saved_frame, access, original);
                if (!captured)
                {
                    auto encoded = mapped_error(access, captured.status());
                    return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
                }
                if (captured.value().original != TcpDescriptionKind::retained_tcp)
                {
                    const bool bad_fd = captured.value().original == TcpDescriptionKind::bad_fd;
                    const auto number = access.linux_personality()
                        ? *Constants::errno_number(bad_fd ? lu::UdpErrno::e_badf : lu::UdpErrno::e_notsock)
                        : bad_fd ? 9u : 88u;
                    return publish(kernel, access, {0, number});
                }
                const Request request{captured.value().operation, captured.value().linux, captured.value().arguments};
                auto result = execute(kernel, pid, access, original, request);
                return result ? publish(kernel, access, result.value()) : result.status();
            }
            i32 original_fd{};
            if (access.operation() != Call::socket)
            {
                // Direct signatures own fd in a saved register, including
                // O32. Accept socketcall selection belongs to the shared path.
                original_fd = static_cast<i32>(static_cast<u32>(first_argument(saved_frame, access)));
                if (!access.live()) return Status::denied("TCP original capture lost image after fd head copy");
                const auto *current = kernel.scheduler().find(pid);
                if (!current) return Status::denied("TCP captured process disappeared before original selection");
                auto selected = original.acquire(*current, original_fd);
                if (!selected)
                {
                    auto encoded = mapped_error(access, selected.status());
                    return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
                }
                if (selected.value() != TcpDescriptionKind::retained_tcp)
                {
                    const auto number = access.linux_personality()
                        ? *Constants::errno_number(selected.value() == TcpDescriptionKind::bad_fd ? lu::UdpErrno::e_badf : lu::UdpErrno::e_notsock)
                        : selected.value() == TcpDescriptionKind::bad_fd ? 9u : 88u;
                    return publish(kernel, access, {0, number});
                }
                if (auto status = access.bind_original(original, original_fd); !status) return status;
            }
            auto request = decode(saved_frame, access);
            if (!request)
            {
                auto encoded = mapped_error(access, request.status());
                return encoded ? publish(kernel, access, {0, encoded.value()}) : encoded.status();
            }
            auto result = execute(kernel, pid, access, original, request.value());
            if (result && request.value().operation == Call::write && request.value().linux &&
                result.value().error == lu::tcp_errno_epipe[profile])
            {
                if (!access.live()) return Status::denied("TCP SIGPIPE lost captured image/descriptor");
                auto *current = kernel.scheduler().find(pid);
                if (!current) return Status::denied("TCP SIGPIPE captured process disappeared");
                queue_pipe_sigpipe(kernel, *current, access.tid());
                if (!runtime_failure) return runtime_failure;
            }
            return result ? publish(kernel, access, result.value()) : result.status();
        }();
        // Publication happens above while the original OFD is still held.
        // Consume capture authority before the only fallible final release.
        access.reset();
        const auto accept_retired = prepared_accept_ ? prepared_accept_->retire() : Status{};
        prepared_accept_.reset();
        const auto released = original.release();
        if (!accept_retired) { started = false; return accept_retired; }
        if (!released) { started = false; return released; }
        return operation;
    }
  private:
    static u64 first_argument(const hw::UserFrame &frame, const Access &access)
    {
        if (!access.linux_personality()) return hw::syscall(frame).arguments[0];
        using Codec = lu::Codec<lu::configured_native_profile()>;
        return lu::NativeBridge<lu::configured_native_profile()>::snapshot(frame).integer[
            Codec::profile.argument_registers[0]] & Codec::word_mask;
    }
    static usize argument_count(Call call)
    {
        if (call == Call::getsockopt || call == Call::setsockopt) return 5;
        if (call == Call::sendto || call == Call::recvfrom) return 6;
        if (call == Call::accept4) return 4;
        if (call == Call::listen || call == Call::shutdown) return 2;
        return 3;
    }
    static Result<Request> decode(const hw::UserFrame &frame, Access &access)
    {
        if (!access.live()) return Status::denied("TCP decode lost captured image");
        Request request{access.operation(), access.linux_personality(), {}};
        if (!request.linux)
        {
            const auto copied = hw::syscall(frame);
            if (copied.number != access.raw_syscall()) return Status::denied("native TCP syscall changed after capture");
            for (usize at = 0; at < request.arguments.size(); ++at) request.arguments[at] = copied.arguments[at];
            if (request.operation == Call::accept) request.arguments[3] = 0;
            return request;
        }
        using Codec = lu::Codec<lu::configured_native_profile()>;
        const auto snapshot = lu::NativeBridge<lu::configured_native_profile()>::snapshot(frame);
        const u64 sp = static_cast<u64>(frame.sp) & Codec::word_mask;
        const u64 number = snapshot.integer[Codec::profile.number_register];
        const auto route = AcceptCall::route(snapshot);
        if (number != access.raw_syscall() || route.socketcall || route.operation != request.operation)
            return Status::denied("Linux TCP operation changed after capture");
        const usize count = argument_count(request.operation);
        std::array<u8, 24> stack{};
        if constexpr (lu::configured_native_profile() == lu::Profile::mips)
        {
            if (count > 4)
            {
                const usize size = (count - 4) * 4;
                if (sp > Codec::word_mask - 16 || size - 1 > Codec::word_mask - (sp + 16))
                    return Status::fault("TCP O32 stack argument interval wraps");
                if (auto status = access.read(sp + 16, {reinterpret_cast<std::byte *>(stack.data() + 16), size}); !status)
                    return status;
            }
        }
        auto decoded = Codec::decode_call(snapshot, count, stack);
        if (!decoded || !access.live()) return decoded ? Status::denied("TCP copied argument authority expired") : decoded.status();
        request.arguments = decoded.value().arguments;
        return request;
    }
    static Result<u32> mapped_error(const Access &access, Status status)
    {
        if (access.linux_personality())
        {
            auto value = net::LinuxTcpStreamAdapter<lu::configured_native_profile()>::error_number(status.code());
            if (value) return *value;
            return Status::unsupported("TCP status has no exact target errno mapping");
        }
        if (status.code() == StatusCode::unsupported) return u32{95};
        return static_cast<u32>(syscall::ErrnoMapper::errno_for(status));
    }
    static u32 error(const Request &request, lu::UdpErrno code, u32 native)
    { return request.linux ? *Constants::errno_number(code) : native; }
    static u32 tcp_error(const Request &request, const std::array<u32, static_cast<usize>(lu::Profile::count)> &values, u32 native)
    { return request.linux ? values[profile] : native; }
    static Result<lu::Return> translated(Access &access, Result<usize> result)
    {
        if (result) return lu::Return{result.value(), 0};
        auto number = mapped_error(access, result.status());
        return number ? Result<lu::Return>{lu::Return{0, number.value()}} : Result<lu::Return>{number.status()};
    }
    static Result<net::tcp::Endpoint> address(Access &access, const Request &request)
    {
        const i32 length = static_cast<i32>(static_cast<u32>(request.arguments[2]));
        if (length < 16 || length > 128 || (!request.linux && length != 16))
            return Status::invalid_argument("TCP sockaddr length is outside this profile");
        std::array<std::byte, 128> bytes{};
        if (auto status = access.read(request.arguments[1], std::span{bytes}.first(static_cast<usize>(length))); !status) return status;
        std::array<u8, 128> copied{};
        for (usize at = 0; at < static_cast<usize>(length); ++at) copied[at] = std::to_integer<u8>(bytes[at]);
        auto endpoint = lu::UdpWire<lu::configured_native_profile()>::decode_address(std::span{copied}.first(static_cast<usize>(length)));
        if (!endpoint) return endpoint.status();
        if (endpoint.value().family != 2) return Status::unsupported("TCP sockaddr family is not IPv4");
        return net::tcp::Endpoint{endpoint.value().address, endpoint.value().port};
    }
    Result<lu::Return> execute(Kernel &kernel, u64 pid, Access &access,
        TcpOriginalDescription &original, const Request &request)
    {
        const auto &a = request.arguments;
        if (request.operation == Call::socket)
        {
            const u32 type = static_cast<u32>(a[1]);
            const u32 stream = request.linux ? Constants::sock_stream : OK_SOCK_STREAM;
            const u32 nonblock = request.linux ? Constants::sock_nonblock : OK_SOCK_NONBLOCK;
            const u32 cloexec = request.linux ? Constants::sock_cloexec : OK_SOCK_CLOEXEC;
            const u32 mask = request.linux ? Constants::sock_type_mask : 0xf;
            if (static_cast<i32>(static_cast<u32>(a[0])) != 2)
                return lu::Return{0, error(request, lu::UdpErrno::e_afnosupport, 97)};
            if ((type & mask) != stream || (type & ~(mask | nonblock | cloexec)) ||
                (static_cast<i32>(static_cast<u32>(a[2])) != 0 && static_cast<i32>(static_cast<u32>(a[2])) != 6))
                return lu::Return{0, error(request, lu::UdpErrno::e_opnotsupp, 95)};
            if (auto status = kernel.activate_native_tcp(); !status) return translated(access, status);
            if (!access.live()) return Status::denied("TCP socket lost image during link activation");
            auto opened = kernel.native_tcp_operations().open(pid, type & nonblock, type & cloexec);
            return translated(access, opened ? Result<usize>{static_cast<usize>(opened.value())} : Result<usize>{opened.status()});
        }
        const i32 fd = static_cast<i32>(static_cast<u32>(a[0]));
        if (!access.live() || !original.matches(pid, fd))
            return Status::denied("TCP decoded operation lost its original descriptor");
        const bool nonblocking = original.flags() & net::TcpStreamOperations::nonblock;
        u32 semantic_error{};
        const auto result = [&]() -> Result<usize> {
            const auto handle = original.handle();
            auto &service = kernel.native_tcp();
            if (request.operation == Call::accept || request.operation == Call::accept4)
            {
                if (prepared_accept_) return Status::busy("TCP runtime already owns a prepared accept");
                net::TcpAcceptCopied<lu::configured_native_profile()> copied{kernel.native_tcp_operations()};
                auto prepared = copied.prepare(pid, fd, a[1], a[2], static_cast<u32>(a[3]),
                    request.operation == Call::accept4, request.linux, access, original);
                if (!prepared) return prepared.status();
                if (!access.live()) return Status::denied("TCP accept lost captured authority before commit");
                prepared_accept_.emplace(std::move(prepared.value()));
                auto accepted = prepared_accept_->commit();
                return accepted ? Result<usize>{static_cast<usize>(accepted.value())} : Result<usize>{accepted.status()};
            }
            if (request.operation == Call::bind)
            {
                auto local = address(access, request); if (!local) return local.status();
                if (local.value().address != kernel.native_udp().link().local_ip)
                { semantic_error = error(request, lu::UdpErrno::e_addrnotavail, 99); return Status::fault(); }
                const auto status = service.bind(handle, local.value()); return status ? Result<usize>{0} : Result<usize>{status};
            }
            if (request.operation == Call::listen)
            {
                const i32 requested = static_cast<i32>(static_cast<u32>(a[1]));
                const usize backlog = requested <= 0 ? 1 : static_cast<usize>(requested);
                const auto status = service.listen(handle, backlog); return status ? Result<usize>{0} : Result<usize>{status};
            }
            if (request.operation == Call::connect)
            {
                if (!nonblocking) return Status::unsupported("blocking TCP connect requires its captured continuation");
                auto state = service.snapshot(handle); if (!state) return state.status();
                if (state.value().endpoint)
                {
                    if (state.value().endpoint->kind != net::tcp::HandleKind::connection)
                        return Status::invalid_argument("TCP listener cannot connect");
                    auto protocol = service.endpoints().state(*state.value().endpoint); if (!protocol) return protocol.status();
                    semantic_error = protocol.value().transport.state == net::tcp::State::syn_sent
                        ? tcp_error(request, lu::tcp_errno_ealready, 114) : tcp_error(request, lu::tcp_errno_eisconn, 106);
                    return Status::fault();
                }
                auto peer = address(access, request); if (!peer) return peer.status();
                if (peer.value().address != kernel.native_udp().link().peer_ip)
                { semantic_error = error(request, lu::UdpErrno::e_netunreach, 101); return Status::fault(); }
                auto now = kernel.native_tcp_now(); if (!now) return now.status();
                if (!access.live()) return Status::denied("TCP connect lost image during clock acquisition");
                auto iss = kernel.native_tcp_iss(state.value().local, peer.value()); if (!iss) return iss.status();
                if (!access.live()) return Status::denied("TCP connect lost image during entropy acquisition");
                const auto status = service.connect(handle, peer.value(), iss.value(), now.value());
                if (status) semantic_error = tcp_error(request, lu::tcp_errno_einprogress, 115);
                return status ? Result<usize>{0} : Result<usize>{status};
            }
            // Defer unsupported operations before touching connection-only state;
            // a listener is a valid stream but not a connection handle.
            if (request.operation != Call::shutdown && request.operation != Call::read && request.operation != Call::write)
                return Status::unsupported("TCP operation requires the next checked continuation slice");
            auto stream = service.snapshot(handle); if (!stream) return stream.status();
            if (!stream.value().endpoint || stream.value().endpoint->kind != net::tcp::HandleKind::connection)
            { semantic_error = tcp_error(request, lu::tcp_errno_enotconn, 107); return Status::fault(); }
            auto protocol = service.endpoints().state(*stream.value().endpoint); if (!protocol) return protocol.status();
            const auto &state = protocol.value().transport;
            if (state.failure != net::tcp::Failure::none)
            {
                if (state.failure == net::tcp::Failure::refused) semantic_error = tcp_error(request, lu::tcp_errno_econnrefused, 111);
                else if (state.failure == net::tcp::Failure::timed_out) semantic_error = tcp_error(request, lu::tcp_errno_etimedout, 110);
                else if (state.failure == net::tcp::Failure::reset) semantic_error = tcp_error(request, lu::tcp_errno_econnreset, 104);
                else semantic_error = tcp_error(request, lu::tcp_errno_econnaborted, 103);
                return Status::fault();
            }
            if (request.operation == Call::shutdown)
            {
                if (static_cast<i32>(static_cast<u32>(a[1])) != OK_SHUT_WR) return Status::unsupported("only TCP write shutdown is supported");
                const auto status = service.shutdown_write(handle); return status ? Result<usize>{0} : Result<usize>{status};
            }
            const u64 ssize_max = request.linux ? (lu::Codec<lu::configured_native_profile()>::word_mask >> 1) : (~uptr{0} >> 1);
            if (a[2] > ssize_max) return Status::invalid_argument("TCP I/O count exceeds target ssize range");
            if (!a[2]) return usize{0};
            const usize size = a[2] < payload_.size() ? static_cast<usize>(a[2]) : payload_.size();
            if (state.state == net::tcp::State::syn_sent || state.state == net::tcp::State::syn_received)
                return Status::would_block("TCP handshake is not complete");
            if (request.operation == Call::read)
            {
                auto preview = service.peek(handle, std::span{payload_}.first(size)); if (!preview) return preview.status();
                if (preview.value())
                    if (auto status = access.write(a[1], {payload_.data(), preview.value()}); !status) return status;
                auto consumed = service.read(handle, std::span{payload_}.first(preview.value()));
                if (!consumed || consumed.value() != preview.value()) return Status::fault("TCP bytes changed after checked output");
                return consumed;
            }
            if (state.write_shutdown || (state.state != net::tcp::State::established && state.state != net::tcp::State::close_wait))
            { semantic_error = tcp_error(request, lu::tcp_errno_epipe, 32); return Status::fault(); }
            if (auto status = access.read(a[1], std::span{payload_}.first(size)); !status) return status;
            return service.write(handle, {payload_.data(), size});
        }();
        if (!access.live()) return Status::denied("TCP operation lost captured image/original descriptor before publication");
        return semantic_error ? Result<lu::Return>{lu::Return{0, semantic_error}} : translated(access, result);
    }
    static Status publish(Kernel &kernel, Access &access, lu::Return value)
    {
        if (!access.live()) return Status::denied("TCP result lost captured image authority");
        auto *process = kernel.scheduler().find(access.pid());
        auto *thread = kernel.scheduler().find_thread(access.pid(), access.tid());
        if (!process || !thread || kernel.scheduler().current_thread() != thread)
            return Status::denied("TCP result lost its original captured process/thread/frame");
        if (value.error > 4095) return Status::invalid_argument("TCP result error exceeds target range");
        thread->syscall_pending = false;
        const i64 result = value.error ? -static_cast<i64>(value.error) : static_cast<i64>(value.value);
        return access.linux_personality() ? set_linux_udp_result(*process, thread->native_frame, result)
                                          : set_result(*process, thread->native_frame, result);
    }
    std::optional<net::PreparedTcpAccept> prepared_accept_{};
    std::array<std::byte, 1024> payload_{};
    bool busy_{};
};
TcpImmediateRuntime tcp_immediate_runtime;

bool tcp_immediate_frame(Kernel &kernel, const sched::ProcessControlBlock &process, const sched::ThreadControlBlock &thread)
{
    return process.resources().personality == Personality::linux
        ? linux_tcp_direct_call<lu::configured_native_profile()>(thread.native_frame, kernel.scheduler(), process) != lu::TcpCall::none
        : native_tcp_call(hw::syscall(thread.native_frame), kernel.scheduler(), process) != lu::TcpCall::none;
}
