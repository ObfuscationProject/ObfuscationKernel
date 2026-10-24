// Included in linux_pipe_tests.cpp: actual adapters with synthetic hosted copy mappings.
template <lu::Profile P, bool Native = false>
void unix_messages(Kernel &kernel, sched::ProcessId pid, usize number_slot, std::array<usize, 4> slots)
{
    using C = lu::UnixMessageCodec<P, Native>;
    const auto before = kernel.scheduler().open_description_count();
    const auto pair = kernel.posix().unix_socketpair(posix::o_NONBLOCK); CHECK(pair);
    const auto pipe = kernel.posix().pipe(posix::o_NONBLOCK); CHECK(pipe);
    auto &resources = kernel.scheduler().find(pid)->resources();
    const auto id = resources.files.entry(pipe.value()[0])->description;
    constexpr usize header_at = 512, vector_at = 640, control_at = 768, data_at = 896, legacy_at = 1024;
    auto memory = [](usize offset, usize length) {
        return std::span<u8>{reinterpret_cast<u8 *>(mock::memory.data()) + offset, length};
    };
    auto word = [&](usize at, u64 value, usize length = C::word_bytes) { C::store(memory(at, length), value); };
    auto initialize = [&](usize capacity, usize rights_count = 1, bool sending = false) {
        mock::memory.fill(std::byte{});
        word(header_at + C::vectors_offset, hw::user_begin + vector_at);
        word(header_at + C::vector_count_offset, 1);
        word(header_at + C::control_offset, hw::user_begin + control_at);
        word(header_at + C::control_length_offset, capacity);
        word(vector_at, hw::user_begin + data_at); word(vector_at + C::word_bytes, 4);
        if (sending)
        {
            word(control_at, C::cmsg_header_bytes + rights_count * 4);
            word(control_at + C::word_bytes, C::socket_level, 4);
            word(control_at + C::word_bytes + 4, C::rights_type, 4);
            for (usize index = 0; index < rights_count; ++index)
                word(control_at + C::cmsg_header_bytes + index * 4, pipe.value()[0], 4);
            mock::memory[data_at] = std::byte{'r'}; mock::memory[data_at + 1] = std::byte{'i'};
            mock::memory[data_at + 2] = std::byte{'g'}; mock::memory[data_at + 3] = std::byte{'h'};
        }
    };
    auto call = [&](bool receive, u64 flags = 0, bool legacy = false) {
        const auto fd = pair.value()[receive ? 1 : 0];
        bool complete = true;
        i64 result{};
        if constexpr (Native)
        {
            hw::Syscall request{}; request.number = receive ? OK_SYS_RECVMSG : OK_SYS_SENDMSG;
            request.arguments = {static_cast<u64>(fd), hw::user_begin + header_at, flags, 0, 0, 0};
            result = native::dispatch(kernel, pid, request, complete);
        }
        else
        {
            hw::UserFrame frame{};
            if (legacy)
            {
                frame.registers[number_slot] = C::numbers.socketcall;
                frame.registers[slots[0]] = receive ? 17 : 16;
                frame.registers[slots[1]] = hw::user_begin + legacy_at;
                word(legacy_at, fd, 4); word(legacy_at + 4, hw::user_begin + header_at, 4); word(legacy_at + 8, flags, 4);
            }
            else
            {
                frame.registers[number_slot] = receive ? C::numbers.recvmsg : C::numbers.sendmsg;
                frame.registers[slots[0]] = fd; frame.registers[slots[1]] = hw::user_begin + header_at;
                frame.registers[slots[2]] = flags;
            }
            const auto copies = mock::copies;
            CHECK(native::linux_udp_immediate_operation_for_frame<P>(frame) == native::LinuxUdpImmediateOperation::none);
            CHECK(mock::copies == copies);
            result = user::linux_bootstrap::dispatch<P>(kernel, pid, frame, complete,
                [&](hw::Syscall request, bool &done) { return native::dispatch(kernel, pid, request, done); });
            CHECK(user::linux_bootstrap::complete<P>(frame, result));
        }
        CHECK(complete); return result;
    };
    auto read_control_fd = [&] {
        return static_cast<i32>(C::load(memory(control_at + C::cmsg_header_bytes, 4)));
    };
    initialize(C::cmsg_header_bytes + 4, 1, true);
    CHECK(call(false, 0x10000) == -95);
    word(control_at, C::cmsg_header_bytes - 1); CHECK(call(false) == -22);
    word(control_at, C::cmsg_header_bytes + 4); word(control_at + C::word_bytes, C::socket_level + 1, 4);
    CHECK(call(false) < 0 && kernel.scheduler().open_description(id)->references == 1);
    initialize(C::cmsg_header_bytes + 4, 1, true); CHECK(call(false) == 4);
    for (usize fault = 1; fault <= 3; ++fault)
    {
        initialize(C::max_control_bytes);
        mock::fail_output_at = mock::outputs + fault;
        CHECK(call(true, C::cmsg_cloexec) == -14); mock::fail_output_at = 0;
        CHECK(resources.files.open_count() == 7 && !resources.unix_receive_operations);
        CHECK(kernel.scheduler().open_description(id)->references == 2 && kernel.scheduler().open_description(id)->unix_queue_references == 1);
        for (const auto &slot : resources.files.entries()) CHECK(!slot.reservation);
    }
    initialize(C::max_control_bytes); CHECK(call(true, C::cmsg_cloexec) == 4);
    CHECK(mock::memory[data_at] == std::byte{'r'} && mock::memory[data_at + 3] == std::byte{'h'});
    CHECK(C::load(memory(header_at + C::flags_offset, 4)) == C::cmsg_cloexec);
    CHECK(C::load(memory(control_at, C::word_bytes)) == C::cmsg_header_bytes + 4);
    const auto received = read_control_fd(); CHECK(resources.files.entry(received)->description == id);
    CHECK(kernel.posix().fcntl(received, posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    CHECK(kernel.posix().close(received));
    initialize(C::cmsg_header_bytes + 8, 2, true); CHECK(call(false) == 4);
    initialize(C::cmsg_header_bytes + 4); CHECK(call(true) == 4);
    CHECK(C::load(memory(header_at + C::flags_offset, 4)) == C::control_truncated);
    CHECK(kernel.posix().close(read_control_fd()));
    CHECK(kernel.scheduler().open_description(id)->references == 1);
    initialize(0); word(vector_at + C::word_bytes, 0);
    CHECK(call(false) == 0);
    const std::array<std::byte, 1> short_data{std::byte{'s'}};
    CHECK(kernel.posix().write(pair.value()[0], short_data).value() == 1);
    initialize(C::max_control_bytes); word(header_at + C::vector_count_offset, 2);
    word(header_at + C::control_offset, 1); // No ancillary output touches this inaccessible storage.
    word(vector_at, hw::user_begin + mock::memory.size() - 1); word(vector_at + C::word_bytes, 100);
    word(vector_at + C::word_bytes * 2, 1); word(vector_at + C::word_bytes * 3, 100);
    CHECK(call(true) == 1);
    CHECK(mock::memory.back() == std::byte{'s'} && C::load(memory(header_at + C::control_length_offset, C::word_bytes)) == 0);
    if constexpr (!Native && C::numbers.socketcall)
    {
        initialize(C::cmsg_header_bytes + 4, 1, true); CHECK(call(false, C::dontwait, true) == 4);
        initialize(C::max_control_bytes); CHECK(call(true, C::dontwait, true) == 4);
        CHECK(kernel.posix().close(read_control_fd()));
    }
    initialize(0); word(vector_at, C::maximum_address - 1); word(vector_at + C::word_bytes, 4);
    CHECK(call(true) == -14);
    std::array<u8, C::max_control_bytes> encoded{};
    std::array<i32, 1> fd_values{3};
    CHECK(!C::encode_rights({encoded.data(), 1}, fd_values, C::max_control_bytes));
    CHECK(C::descriptor_capacity(C::cmsg_header_bytes + 3) == 0);
    CHECK(kernel.posix().close(pair.value()[0]) && kernel.posix().close(pair.value()[1]));
    CHECK(kernel.posix().close(pipe.value()[0]) && kernel.posix().close(pipe.value()[1]));
    CHECK(resources.files.open_count() == 3 && kernel.scheduler().open_description_count() == before);
    std::printf("PASS %s UNIX messages %s: endian, direct/legacy, failed copy rollback and CLOEXEC result\n",
        Native ? "native" : "Linux", lu::Codec<P>::profile.name);
}
