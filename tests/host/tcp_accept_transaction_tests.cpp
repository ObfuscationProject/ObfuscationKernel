#include <ok/net/tcp_accept_copied.hpp>
#include "../../src/user/tcp_original_ofd.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>
using namespace ok;
using namespace ok::net;
namespace lu = ok::abi::linux_user;
namespace
{
usize checks{};
u64 identity{61000};
#define CHECK(value) do { ++checks; if (!static_cast<bool>(value)) { std::fprintf(stderr, "TCP_ACCEPT_TRANSACTION_FAIL line=%d check=%zu: %s\n", __LINE__, checks, #value); std::abort(); } } while(false)
constexpr tcp::Endpoint server{{10, 0, 0, 2}, 8080}, peer{{10, 0, 0, 3}, 32001};
struct Iss { Result<u32> operator()(tcp::Endpoint, tcp::Endpoint) { return u32{500}; } };
struct Packet
{
    std::array<std::byte, 1500> bytes{}; usize length{};
    wire::Bytes view() const { return {bytes.data(), length}; }
};
Packet packet(u32 sequence, u32 acknowledgment, u8 flags)
{
    Packet out;
    auto built = wire::build_tcp(out.bytes, peer.address, server.address, 1, 64,
        {peer.port, server.port, sequence, acknowledgment, flags, 4096}, {}, {});
    CHECK(built); out.length = built.value(); return out;
}
struct Fixture
{
    sched::Scheduler scheduler{};
    NativeTcpStreamService service;
    TcpStreamOperations operations{scheduler, service};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(4096, 32768)};
    Status failure{};
    u64 pid{}; i32 fd{}; u16 id{}; TcpStreamHandle listener{};
    explicit Fixture(TcpStreamCapacity capacity = {}) : service{tcp::TableIdentity{identity++}, capacity}
    {
        CHECK(scheduler.set_native_tcp_service(service));
        auto created = scheduler.create_process("accept-owner", context); CHECK(created); pid = created.value();
        CHECK(scheduler.set_runnable(pid)); CHECK(scheduler.activate(pid));
        auto opened = operations.open(pid, true, true); CHECK(opened); fd = opened.value();
        id = process().fd_table().entry(fd)->description; listener = scheduler.open_description(id)->native_tcp_handle;
        CHECK(operations.bind(pid, fd, server)); CHECK(operations.listen(pid, fd, 2));
    }
    ~Fixture()
    { CHECK(operations.status()); CHECK(failure); CHECK(scheduler.drain_all_resources()); CHECK(service.endpoints().retire()); }
    sched::ProcessControlBlock &process() { return *scheduler.find(pid); }
    void completed()
    {
        Iss iss; auto syn = service.endpoints().ingest(packet(10, 0, wire::tcp_syn).view(), 0, iss); CHECK(syn && syn.value().matched);
        Packet out; auto response = service.endpoints().prepare(out.bytes, 0); CHECK(response && response.value());
        out.length = response.value()->length; CHECK(service.endpoints().complete(response.value()->ticket, true, 0));
        auto ip = wire::ipv4(out.view()); CHECK(ip); auto tcp = wire::tcp(ip.value()); CHECK(tcp);
        auto ack = service.endpoints().ingest(packet(11, tcp.value().fields.sequence + 1, wire::tcp_ack).view(), 1, iss);
        CHECK(ack && ack.value().matched); CHECK(service.endpoints().pools().unaccepted == 1);
    }
    void capture(user::native::TcpOriginalDescription &original)
    { auto selected = original.acquire(process(), fd); CHECK(selected && selected.value() == user::native::TcpDescriptionKind::retained_tcp); }
    void unchanged_queue()
    {
        CHECK(service.endpoints().pools().unaccepted == 1 && service.endpoints().pools().accept_holds == 0);
        CHECK(service.prepared_accepts() == 0 && service.active_streams() == 1);
        CHECK(process().resources().prepared_descriptor_operations == 0 && process().fd_table().open_count() == 4);
    }
};
struct Copy
{
    std::array<std::byte, 4096> bytes{};
    bool authority{true}; usize calls{}, fail_at{}, revoke_at{}, close_at{}, guard_calls{};
    Fixture *fixture{};
    bool live() const { return authority; }
    Status visit()
    {
        ++calls;
        if (fixture && guard_calls)
        {
            CHECK(fixture->scheduler.drain_resources(fixture->process().resources()).code() == StatusCode::busy);
            CHECK(fixture->scheduler.drain_all_resources().code() == StatusCode::busy);
            CHECK(!fixture->scheduler.fork_process(fixture->pid));
            CHECK(fixture->scheduler.replace_image(fixture->pid, fixture->context, {}).code() == StatusCode::busy);
            CHECK(fixture->scheduler.exit_process(fixture->pid, 0).code() == StatusCode::busy);
            CHECK(fixture->scheduler.reap_process(fixture->pid).code() == StatusCode::busy);
            CHECK(!fixture->operations.open(fixture->pid));
        }
        if (calls == revoke_at) authority = false;
        if (calls == close_at)
        {
            const auto old = fixture->process().fd_table().entry(fixture->fd)->installation;
            CHECK(fixture->scheduler.close_descriptor(fixture->process().resources(), fixture->fd));
            CHECK(fixture->scheduler.install_descriptor(fixture->process().resources(), fixture->fd, fixture->id));
            CHECK(fixture->process().fd_table().entry(fixture->fd)->installation != old);
        }
        return calls == fail_at ? Status::fault("injected partial copy refusal") : Status{};
    }
    Status preflight(u64 address, usize length, bool)
    {
        if (auto status = visit(); !status) return status;
        return address >= 4096 && address - 4096 <= bytes.size() && length <= bytes.size() - (address - 4096)
            ? Status{} : Status::fault("unmapped test copy interval");
    }
    Status read(u64 address, std::span<std::byte> output)
    {
        const auto status = visit();
        if (!status) return status;
        const usize at = static_cast<usize>(address - 4096);
        for (usize index = 0; index < output.size(); ++index) output[index] = bytes[at + index];
        return {};
    }
    Status write(u64 address, wire::Bytes input)
    {
        const auto status = visit();
        const usize at = static_cast<usize>(address - 4096);
        // A failed output can change user bytes; it still cannot commit an fd.
        for (usize index = 0; index < (status ? input.size() : usize{1}); ++index) bytes[at + index] = input[index];
        return status;
    }
    template <lu::Profile Selected> void length(i32 value)
    {
        // Construct a raw negative input too; the output encoder correctly
        // refuses negative socklen and cannot be this adversarial fixture.
        std::array<u8, 4> record{};
        for (usize at = 0; at < 4; ++at)
        {
            const auto shift = lu::Codec<Selected>::profile.big_endian ? (3 - at) * 8 : at * 8;
            record[at] = static_cast<u8>(static_cast<u32>(value) >> shift);
        }
        for (usize at = 0; at < 4; ++at) bytes[256 + at] = std::byte{record[at]};
    }
};
using Original = user::native::TcpOriginalDescription;
constexpr auto host_profile = lu::Profile::x86_64;
void successful_publication_and_lifecycle()
{
    auto owner = std::make_unique<Fixture>(); auto &f = *owner; f.completed();
    Original original{f.scheduler, f.service, f.failure}; f.capture(original);
    Copy copy; copy.fixture = &f; copy.guard_calls = 1; copy.length<host_profile>(16);
    TcpAcceptCopied<host_profile> adapter{f.operations};
    auto prepared = adapter.prepare(f.pid, f.fd, 4608, 4352, 0, false, true, copy, original); CHECK(prepared);
    CHECK(copy.calls == 6 && f.service.prepared_accepts() == 1 && f.service.endpoints().pools().unaccepted == 1);
    CHECK(f.scheduler.open_description(f.id)->references == 3);
    const auto reserved = prepared.value().descriptor(); CHECK(!f.process().fd_table().entry(reserved)->used);
    auto accepted = prepared.value().commit(); CHECK(accepted && accepted.value() == reserved); CHECK(!prepared.value().commit());
    const auto *slot = f.process().fd_table().entry(reserved); const auto accepted_id = slot->description;
    CHECK(slot->used && !slot->close_on_exec && slot->installation && f.scheduler.open_description(accepted_id)->flags == 2);
    CHECK(f.scheduler.open_description(accepted_id)->references == 1 && f.service.endpoints().pools().unaccepted == 0);
    CHECK(copy.bytes[512] == std::byte{2} && copy.bytes[513] == std::byte{0});
    CHECK(copy.bytes[514] == std::byte{0x7d} && copy.bytes[515] == std::byte{1});
    CHECK(copy.bytes[516] == std::byte{10} && copy.bytes[519] == std::byte{3} && copy.bytes[256] == std::byte{16});
    CHECK(f.scheduler.open_description(f.id)->references == 3); CHECK(prepared.value().retire()); CHECK(original.release());
    CHECK(f.scheduler.open_description(f.id)->references == 1);
    CHECK(f.scheduler.install_descriptor(f.process().resources(), 5, accepted_id));
    auto child = f.scheduler.fork_process(f.pid); CHECK(child);
    CHECK(f.scheduler.open_description(accepted_id)->references == 4);
    CHECK(f.scheduler.close_descriptor(f.process().resources(), reserved)); CHECK(f.scheduler.close_descriptor(f.process().resources(), 5));
    CHECK(f.scheduler.exit_process(child.value(), 0)); CHECK(!f.scheduler.open_description(accepted_id));
}
void callback_refusal_and_reinstallation()
{
    for (usize boundary = 1; boundary <= 6; ++boundary)
        for (usize action = 0; action < 3; ++action)
        {
            auto owner = std::make_unique<Fixture>(); auto &f = *owner; f.completed();
            Original original{f.scheduler, f.service, f.failure}; f.capture(original);
            Copy copy; copy.fixture = &f; copy.length<host_profile>(16);
            if (action == 0) copy.fail_at = boundary;
            if (action == 1) copy.revoke_at = boundary;
            if (action == 2) copy.close_at = boundary;
            TcpAcceptCopied<host_profile> adapter{f.operations};
            auto failed = adapter.prepare(f.pid, f.fd, 4608, 4352, 0, false, true, copy, original);
            CHECK(!failed && copy.calls == boundary); f.unchanged_queue();
            CHECK(f.scheduler.open_description(f.id)->references == 2); CHECK(original.release());
            CHECK(f.scheduler.open_description(f.id)->references == 1);
        }
}
template <lu::Profile Selected> void profile_controls()
{
    using Wire = lu::UdpWire<Selected>; using Constants = lu::UdpConstants<Selected>;
    for (i32 size : {0, 3, 16, 64})
    {
        auto owner = std::make_unique<Fixture>(); auto &f = *owner; f.completed();
        Original original{f.scheduler, f.service, f.failure}; f.capture(original);
        Copy copy; copy.bytes.fill(std::byte{0x5a}); copy.length<Selected>(size);
        TcpAcceptCopied<Selected> adapter{f.operations};
        auto prepared = adapter.prepare(f.pid, f.fd, 4608, 4352, Constants::sock_nonblock | Constants::sock_cloexec, true, true, copy, original);
        CHECK(prepared); auto accepted = prepared.value().commit(); CHECK(accepted);
        const auto *slot = f.process().fd_table().entry(accepted.value()); const auto *open = f.scheduler.open_description(slot->description);
        CHECK(slot->close_on_exec && open->flags == (2 | TcpStreamOperations::nonblock));
        std::array<u8, 16> expected{}; CHECK(Wire::encode_address(expected, {2, peer.port, peer.address}));
        for (usize index = 0; index < static_cast<usize>(size < 16 ? size : 16); ++index) CHECK(copy.bytes[512 + index] == std::byte{expected[index]});
        CHECK(copy.bytes[512 + static_cast<usize>(size < 16 ? size : 16)] == std::byte{0x5a});
        std::array<u8, 4> reported{}; for (usize index = 0; index < 4; ++index) reported[index] = std::to_integer<u8>(copy.bytes[256 + index]);
        CHECK(Wire::decode_socklen(reported).value() == 16); CHECK(prepared.value().retire()); CHECK(original.release());
        CHECK(f.scheduler.replace_image(f.pid, f.context, {})); CHECK(!f.process().fd_table().contains(accepted.value()));
    }
    auto owner = std::make_unique<Fixture>(); auto &f = *owner; f.completed();
    Original original{f.scheduler, f.service, f.failure}; f.capture(original); Copy copy; copy.length<Selected>(16);
    TcpAcceptCopied<Selected> adapter{f.operations};
    const u32 invalid = Constants::sock_nonblock == 0x80 ? 0x800 : 0x80;
    CHECK(!adapter.prepare(f.pid, f.fd, 4608, 4352, invalid, true, true, copy, original) && copy.calls == 0); f.unchanged_queue();
    auto null = adapter.prepare(f.pid, f.fd, 0, ~u64{0}, 0, false, false, copy, original); CHECK(null && copy.calls == 0);
    CHECK(null.value().cancel()); f.unchanged_queue(); CHECK(null.value().retire()); CHECK(original.release());
}
void malformed_controls_and_empty_queue()
{
    auto owner = std::make_unique<Fixture>(); auto &f = *owner; f.completed();
    Original original{f.scheduler, f.service, f.failure}; f.capture(original); TcpAcceptCopied<lu::Profile::mips> adapter{f.operations};
    for (usize mode = 0; mode < 5; ++mode)
    {
        Copy copy; copy.length<lu::Profile::mips>(mode == 0 ? -1 : 16);
        const u64 address = mode == 1 ? 4353 : mode == 2 ? 0xfffffff8 : 4608;
        const u64 length_address = mode == 3 ? 0xfffffffe : mode == 4 ? 0 : 4352;
        CHECK(!adapter.prepare(f.pid, f.fd, address, length_address, 0, false, true, copy, original)); f.unchanged_queue();
    }
    CHECK(original.release());
    auto empty = std::make_unique<Fixture>(); auto &e = *empty;
    Original empty_original{e.scheduler, e.service, e.failure}; e.capture(empty_original); Copy copy;
    TcpAcceptCopied<host_profile> empty_adapter{e.operations};
    auto nonblock = empty_adapter.prepare(e.pid, e.fd, 0, 0, 0, false, true, copy, empty_original);
    CHECK(!nonblock && nonblock.status().code() == StatusCode::would_block && copy.calls == 0); CHECK(empty_original.release());
    e.scheduler.open_description(e.id)->flags &= ~TcpStreamOperations::nonblock;
    Original blocking_original{e.scheduler, e.service, e.failure}; e.capture(blocking_original);
    auto blocking = empty_adapter.prepare(e.pid, e.fd, 0, 0, 0, false, true, copy, blocking_original);
    CHECK(!blocking && blocking.status().code() == StatusCode::unsupported); CHECK(blocking_original.release());
}
void capacity_and_shared_tokens()
{
    for (usize mode = 0; mode < 3; ++mode)
    {
        auto owner = std::make_unique<Fixture>(mode == 2 ? TcpStreamCapacity{~u64{0}, 1} : TcpStreamCapacity{}); auto &f = *owner; f.completed();
        if (mode == 0) for (i32 fd = 4; fd < static_cast<i32>(sched::max_process_fds); ++fd) CHECK(f.scheduler.install_descriptor(f.process().resources(), fd, 1));
        // open_description_count reports referenced OFDs, not unpublished slots.
        if (mode == 1) for (usize used = 4; used < sched::max_open_descriptions; ++used) CHECK(f.scheduler.allocate_description({}));
        auto refused = f.operations.prepare_accept(f.pid, f.fd); CHECK(!refused);
        CHECK(f.service.endpoints().pools().unaccepted == 1 && f.service.endpoints().pools().accept_holds == 0 && f.service.prepared_accepts() == 0);
        CHECK(f.process().resources().prepared_descriptor_operations == 0 && f.scheduler.open_description(f.id)->references == 1);
        if (mode == 1) for (u16 id = 3; id < sched::max_open_descriptions; ++id) if (auto *open = f.scheduler.open_description(id); open && !open->references) *open = {};
    }
    auto owner = std::make_unique<Fixture>(); auto &f = *owner; f.completed();
    auto pair = f.scheduler.create_unix_socketpair(f.process().resources()); CHECK(pair);
    const std::array payload{std::byte{'x'}}; const std::array rights{f.fd}; std::array<std::byte, 4> preview{};
    CHECK(f.scheduler.send_unix_rights(f.process().resources(), pair.value()[0], payload, rights));
    auto tcp = f.operations.begin_accept(f.pid, f.fd); CHECK(tcp);
    auto unix = f.scheduler.prepare_unix_receive(f.process().resources(), pair.value()[1], preview, 1); CHECK(unix);
    const auto tcp_fd = tcp.value().descriptor(), unix_fd = unix.value().descriptors()[0];
    CHECK(tcp_fd != unix_fd && f.process().fd_table().entry(tcp_fd)->reservation != f.process().fd_table().entry(unix_fd)->reservation);
    CHECK(!f.scheduler.install_descriptor(f.process().resources(), tcp_fd, f.id));
    const auto reserved_id = [&] { for (u16 id = 3; id < sched::max_open_descriptions; ++id) { const auto *open = f.scheduler.open_description(id); if (open && open->reservation) return id; } return sched::invalid_description; }();
    CHECK(reserved_id != sched::invalid_description && !f.scheduler.retain_description(reserved_id));
    CHECK(!f.scheduler.install_descriptor(f.process().resources(), 20, reserved_id));
    CHECK(!f.scheduler.allocate_description(*f.scheduler.open_description(reserved_id)));
    CHECK(unix.value().commit().value() == 1); CHECK(tcp.value().prepare_child()); auto accepted = tcp.value().commit(); CHECK(accepted);
    CHECK(f.process().fd_table().entry(accepted.value())->installation != f.process().fd_table().entry(unix_fd)->installation);
    CHECK(tcp.value().retire()); CHECK(f.process().resources().unix_receive_operations == 0 && f.process().resources().prepared_descriptor_operations == 0);
}
void reset_and_descriptor_commit_refusal()
{
    // Independent conversations avoid mistaking the endpoint's same-tuple
    // post-reset quiet period for failure of descriptor reservation authority.
    for (usize mode = 0; mode < 2; ++mode)
    {
        auto owner = std::make_unique<Fixture>(); auto &f = *owner; f.completed();
        auto prepared = f.operations.prepare_accept(f.pid, f.fd); CHECK(prepared);
        if (mode == 0)
        {
            Iss iss; CHECK(f.service.endpoints().ingest(packet(11, 501, wire::tcp_rst | wire::tcp_ack).view(), 2, iss));
            CHECK(!prepared.value().commit()); CHECK(prepared.value().retire());
            CHECK(f.service.endpoints().pools().connections == 0 && f.service.prepared_accepts() == 0);
        }
        else
        {
            const auto installation = f.process().fd_table().entry(f.fd)->installation;
            CHECK(f.scheduler.close_descriptor(f.process().resources(), f.fd)); CHECK(f.scheduler.install_descriptor(f.process().resources(), f.fd, f.id));
            CHECK(f.process().fd_table().entry(f.fd)->installation != installation && !prepared.value().commit());
            CHECK(prepared.value().retire()); f.unchanged_queue();
        }
    }
}
}
int main()
{
    static_assert(!std::is_copy_constructible_v<PreparedTcpAccept> && std::is_nothrow_move_constructible_v<PreparedTcpAccept>);
    successful_publication_and_lifecycle(); callback_refusal_and_reinstallation(); malformed_controls_and_empty_queue();
    profile_controls<lu::Profile::i386>(); profile_controls<lu::Profile::x86_64>(); profile_controls<lu::Profile::aarch64>();
    profile_controls<lu::Profile::arm32>(); profile_controls<lu::Profile::rv64>(); profile_controls<lu::Profile::rv32>();
    profile_controls<lu::Profile::loongarch64>(); profile_controls<lu::Profile::mips>(); profile_controls<lu::Profile::mips64>(); profile_controls<lu::Profile::ppc>();
    capacity_and_shared_tokens(); reset_and_descriptor_commit_refusal();
    std::printf("TCP_ACCEPT_TRANSACTION_PASS checks=%zu profiles=10 syscall_execution=0 device_execution=0\n", checks);
}
