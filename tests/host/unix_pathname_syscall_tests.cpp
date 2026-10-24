// Hosted real VFS/POSIX/syscall-marshaller model; no native trap execution.
#include "ok/user/unix_pathname.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace ok;
namespace
{
usize checks{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "UNIX_PATH_SYSCALL_FAIL line=%d: %s\n", __LINE__, #value); std::abort(); } } while(false)
struct Fixture
{
    fs::VirtualFileSystem vfs;
    sched::Scheduler scheduler;
    driver::ConsoleDriver console;
    posix::PosixService posix;
    std::array<u8, 1024> memory{};
    sched::ProcessId server{}, client{};
    bool fault_output{}, replace_input{}, replace_writable{};
    i32 replaced_fd{}, replacement{};
    Fixture()
    {
        CHECK(console.start()); CHECK(posix.initialize(vfs, console, scheduler));
        auto context = arch::configured_arch_operations().make_user_context({1, 2, 0});
        auto first = scheduler.create_user_process("pathname-server", context); CHECK(first); server = first.value();
        auto second = scheduler.create_user_process("pathname-client", context); CHECK(second); client = second.value();
        CHECK(posix.activate(server)); CHECK(posix.set_identity(1000, 1001)); static_cast<void>(posix.umask(0));
    }
    ~Fixture() { CHECK(scheduler.drain_all_resources()); CHECK(!scheduler.unix_endpoint_count()); }
    u16 id(i32 fd) { return scheduler.find(posix.getpid())->fd_table().entry(fd)->description; }
    Status input(u64 address, std::span<u8> bytes)
    {
        if (address > memory.size() || bytes.size() > memory.size() - address) return Status::fault();
        for (usize i = 0; i < bytes.size(); ++i) bytes[i] = memory[address + i];
        if (replace_input)
        {
            replace_input = false; CHECK(posix.close(replaced_fd));
            auto fresh = posix.unix_socket(posix::o_NONBLOCK); CHECK(fresh); replacement = fresh.value();
        }
        return {};
    }
    Status output(u64 address, std::span<const u8> bytes)
    {
        if (fault_output && address == 200) return Status::fault();
        if (address > memory.size() || bytes.size() > memory.size() - address) return Status::fault();
        for (usize i = 0; i < bytes.size(); ++i) memory[address + i] = bytes[i];
        return {};
    }
    bool writable(u64 address, usize size)
    {
        if (replace_writable)
        {
            replace_writable = false; CHECK(posix.close(replaced_fd));
            auto fresh = posix.unix_socket(posix::o_NONBLOCK); CHECK(fresh); replacement = fresh.value();
        }
        return address <= memory.size() && size <= memory.size() - address;
    }
};
template<abi::linux_user::Profile Profile> void model()
{
    using C = abi::linux_user::UnixPathCodec<Profile>;
    using Call = abi::linux_user::UnixPathCall;
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &f = *::new(storage.data()) Fixture;
    auto call = [&](Call operation, std::array<u64, 6> arguments) {
        return user::native::unix_pathname<Profile>(f.posix, f.scheduler, *f.scheduler.find(f.posix.getpid()), operation, arguments,
            [](std::array<u64, 6> copied) -> Result<std::array<u64, 6>> { return copied; },
            [&](u64 address, std::span<u8> bytes) { return f.input(address, bytes); },
            [&](u64 address, std::span<const u8> bytes) { return f.output(address, bytes); },
            [&](u64 address, usize size) { return f.writable(address, size); });
    };
    constexpr std::string_view path = "/tmp/profile.sock";
    C::store(std::span<u8>{f.memory}.subspan(100, 2), 1);
    for (usize i = 0; i < path.size(); ++i) f.memory[102 + i] = static_cast<u8>(path[i]);
    f.memory[102 + path.size()] = 0;
    const auto length = path.size() + 3;
    const auto listener = call(Call::socket, {1, C::stream | C::nonblock | C::cloexec, 0}); CHECK(listener >= 0);
    CHECK(call(Call::bind, {static_cast<u64>(listener), 100, length}) == 0);
    auto metadata = f.posix.stat(path); CHECK(metadata && metadata.value().type == fs::NodeType::socket && metadata.value().mode == (fs::mode_socket | 0777));
    CHECK(!f.posix.open(path, posix::o_RDONLY));
    CHECK(call(Call::listen, {static_cast<u64>(listener), 2}) == 0);
    CHECK(f.posix.set_identity(2000, 2001));
    CHECK(f.posix.activate(f.client)); CHECK(f.posix.set_identity(3000, 3001));
    const auto client = call(Call::socket, {1, C::stream | C::nonblock, 0}); CHECK(client >= 0);
    CHECK(call(Call::connect, {static_cast<u64>(client), 100, length}) == 0);
    CHECK(f.posix.set_identity(4000, 4001));
    C::store(std::span<u8>{f.memory}.subspan(300, 4), 12);
    CHECK(call(Call::getsockopt, {static_cast<u64>(client), C::sol_socket, C::peercred, 400, 300}) == 0);
    CHECK(C::load(std::span<const u8>{f.memory}.subspan(400, 4)) == f.server);
    CHECK(C::load(std::span<const u8>{f.memory}.subspan(404, 4)) == 1000);
    CHECK(C::load(std::span<const u8>{f.memory}.subspan(408, 4)) == 1001);
    const std::array payload{std::byte{'p'}};
    const std::array<i32, 1> rights{static_cast<i32>(client)};
    CHECK(f.posix.send_unix_rights(static_cast<i32>(client), payload, rights));
    CHECK(f.posix.activate(f.server));
    auto preview = f.posix.prepare_unix_accept(f.id(static_cast<i32>(listener))); CHECK(preview);
    const auto queued = preview.value().description(); preview.value().cancel();
    C::store(std::span<u8>{f.memory}.subspan(300, 4), C::address_capacity);
    f.fault_output = true;
    CHECK(call(Call::accept4, {static_cast<u64>(listener), 200, 300, C::nonblock | C::cloexec}) == -14);
    auto retry = f.posix.prepare_unix_accept(f.id(static_cast<i32>(listener))); CHECK(retry);
    CHECK(retry.value().description() == queued); retry.value().cancel();
    f.fault_output = false;
    const auto accepted = call(Call::accept4, {static_cast<u64>(listener), 200, 300, C::nonblock | C::cloexec}); CHECK(accepted >= 0);
    CHECK(f.id(static_cast<i32>(accepted)) == queued);
    CHECK(f.posix.fcntl(static_cast<i32>(accepted), posix::f_GETFD, 0).value() == posix::fd_CLOEXEC);
    C::store(std::span<u8>{f.memory}.subspan(300, 4), 12);
    CHECK(call(Call::getsockopt, {static_cast<u64>(accepted), C::sol_socket, C::peercred, 400, 300}) == 0);
    CHECK(C::load(std::span<const u8>{f.memory}.subspan(400, 4)) == f.client);
    CHECK(C::load(std::span<const u8>{f.memory}.subspan(404, 4)) == 3000);
    CHECK(C::load(std::span<const u8>{f.memory}.subspan(408, 4)) == 3001);
    CHECK(f.posix.close(static_cast<i32>(accepted))); CHECK(f.posix.close(static_cast<i32>(listener)));
    // The original pin prevents a copy callback from binding a replacement fd.
    const auto original = call(Call::socket, {1, C::stream | C::nonblock, 0}); CHECK(original >= 0);
    f.replaced_fd = static_cast<i32>(original); f.replace_input = true;
    CHECK(call(Call::bind, {static_cast<u64>(original), 100, length}) == -9);
    CHECK(f.replacement == original); CHECK(f.posix.close(f.replacement));
    // A preflight callback may replace an fd too. It must fail before output.
    const auto preflight = call(Call::socket, {1, C::stream | C::nonblock, 0}); CHECK(preflight >= 0);
    f.replaced_fd = static_cast<i32>(preflight); f.replace_writable = true;
    C::store(std::span<u8>{f.memory}.subspan(300, 4), 4);
    for (usize i = 400; i < 404; ++i) f.memory[i] = 0xaa;
    CHECK(call(Call::getsockopt, {static_cast<u64>(preflight), C::sol_socket, C::so_type, 400, 300}) == -9);
    CHECK(f.replacement == preflight);
    for (usize i = 400; i < 404; ++i) CHECK(f.memory[i] == 0xaa);
    CHECK(f.posix.close(f.replacement));
    CHECK(f.posix.activate(f.client)); CHECK(f.posix.close(static_cast<i32>(client)));
    CHECK(f.posix.activate(f.server)); CHECK(f.posix.set_identity(1000, 1001)); CHECK(f.posix.unlink(path));
    f.~Fixture();
}
}
int main()
{
    using P = abi::linux_user::Profile;
    model<P::i386>(); model<P::x86_64>(); model<P::aarch64>(); model<P::arm32>();
    model<P::rv64>(); model<P::rv32>(); model<P::loongarch64>(); model<P::mips>(); model<P::mips64>(); model<P::ppc>();
    std::printf("UNIX_PATH_SYSCALL_MODEL profiles=10 checks=%zu pass=1\n", checks);
}
