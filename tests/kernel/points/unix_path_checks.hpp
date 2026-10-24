#pragma once
#include <ok/fs/vfs.hpp>
#include <ok/sched/scheduler.hpp>
#include <new>

namespace ok::tests
{
namespace unix_path_model
{
struct Fixture
{
    fs::VirtualFileSystem vfs;
    sched::Scheduler scheduler;
};
}
// Canonical endpoint/namespace ownership model. This executes no user syscall.
inline Status check_unix_path_ownership_model()
{
    using namespace unix_path_model;
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new(storage.data()) Fixture;
    const auto result = [&]() -> Status {
        auto &scheduler = fixture.scheduler;
        auto &resources = scheduler.boot_resources();
        const auto baseline = scheduler.open_description_count();
        fs::ResolveContext context{.credentials = {0, 0}, .check_search = true};
        constexpr sched::UnixPeerCredentials server_credentials{11, 12, 13}, client_credentials{21, 22, 23};
        auto listener = scheduler.create_unix_socket(resources, true, 0x0800u);
        if (!listener) return listener.status();
        const auto listener_id = resources.files.entry(listener.value())->description;
        auto path = fixture.vfs.create_socket(context, "/tmp/point.sock", 0700);
        if (!path) return path.status();
        if (auto status = scheduler.bind_unix_socket(listener_id, std::move(path.value())); !status) return status;
        if (auto status = scheduler.listen_unix_socket(listener_id, 1, server_credentials); !status) return status;
        auto client = scheduler.create_unix_socket(resources, false, 0x0800u);
        if (!client) return client.status();
        const auto client_id = resources.files.entry(client.value())->description;
        auto target = fixture.vfs.resolve_socket(context, "/tmp/point.sock");
        if (!target) return target.status();
        if (auto status = scheduler.connect_unix_socket(client_id, target.value(), client_credentials); !status) return status;
        if (auto status = target.value().release(); !status) return status;
        auto readiness = scheduler.unix_socket_readiness(listener_id);
        auto credentials = scheduler.unix_peer_credentials(client_id);
        if (!readiness || !readiness.value().readable || !credentials || credentials.value() != server_credentials)
            return Status::fault("pathname UNIX listener readiness or peer snapshot mismatch");
        const auto *client_open = scheduler.open_description(client_id);
        const std::array input{std::byte{'u'}, std::byte{'n'}, std::byte{'i'}, std::byte{'x'}};
        auto written = scheduler.write_pipe(client_open->unix_write_pipe, input, true, false);
        if (!written || written.value() != input.size()) return Status::fault("pathname UNIX preaccept data failed");
        u16 child_id{};
        {
            auto preview = scheduler.prepare_unix_accept(resources, listener_id, true, 0x0800u);
            if (!preview) return preview.status();
            child_id = preview.value().description();
            if (preview.value().peer_credentials() != client_credentials ||
                scheduler.drain_resources(resources).code() != StatusCode::busy)
                return Status::fault("pathname UNIX prepared accept lost its owner");
            preview.value().cancel();
        }
        auto retry = scheduler.prepare_unix_accept(resources, listener_id, true, 0x0800u);
        if (!retry || retry.value().description() != child_id)
            return Status::fault("pathname UNIX cancelled accept consumed its queued child");
        auto accepted = retry.value().commit();
        if (!accepted) return accepted.status();
        const auto *server_open = scheduler.open_description(child_id);
        if (!server_open || server_open->node || server_open->unix_queue_references ||
            server_open->references != 1 || !resources.files.entry(accepted.value())->close_on_exec)
            return Status::fault("pathname UNIX accept did not transfer its canonical queue owner");
        std::array<std::byte, 4> output{};
        auto received = scheduler.read_pipe(server_open->pipe, output, true);
        if (!received || received.value() != input.size() || output != input)
            return Status::fault("pathname UNIX accept lost preaccept bytes");
        if (auto status = fixture.vfs.unlink("/tmp/point.sock"); !status) return status;
        if (auto status = scheduler.close_descriptor(resources, listener.value()); !status) return status;
        if (auto status = scheduler.close_descriptor(resources, accepted.value()); !status) return status;
        readiness = scheduler.unix_socket_readiness(client_id);
        if (!readiness || !readiness.value().hangup)
            return Status::fault("pathname UNIX accepted peer teardown lost HUP");
        if (auto status = scheduler.close_descriptor(resources, client.value()); !status) return status;
        if (scheduler.unix_endpoint_count() || scheduler.open_description_count() != baseline)
            return Status::fault("pathname UNIX endpoint model leaked ownership");
        // More cycles than either stream or endpoint capacity expose retained
        // listener/pending-child/SCM_RIGHTS cycles and stale binding identities.
        for (usize iteration = 0; iteration <= sched::max_unix_endpoints; ++iteration)
        {
            auto listening = scheduler.create_unix_socket(resources); if (!listening) return listening.status();
            const auto id = resources.files.entry(listening.value())->description;
            auto bound = fixture.vfs.create_socket(context, "/tmp/cycle-point.sock", 0700);
            if (!bound) return bound.status();
            if (auto status = scheduler.bind_unix_socket(id, std::move(bound.value())); !status) return status;
            if (auto status = scheduler.listen_unix_socket(id, 1, server_credentials); !status) return status;
            auto connecting = scheduler.create_unix_socket(resources); if (!connecting) return connecting.status();
            const auto connecting_id = resources.files.entry(connecting.value())->description;
            auto destination = fixture.vfs.resolve_socket(context, "/tmp/cycle-point.sock");
            if (!destination) return destination.status();
            if (auto status = scheduler.connect_unix_socket(connecting_id, destination.value(), client_credentials); !status) return status;
            if (auto status = destination.value().release(); !status) return status;
            const std::array<i32, 1> rights{listening.value()};
            auto sent = scheduler.send_unix_rights(resources, connecting.value(), input, rights);
            if (!sent || sent.value() != input.size()) return Status::fault("pathname UNIX queue-cycle model send failed");
            if (auto status = scheduler.close_descriptor(resources, listening.value()); !status) return status;
            if (auto status = scheduler.close_descriptor(resources, connecting.value()); !status) return status;
            if (auto status = scheduler.collect_unix_rights(); !status) return status;
            if (scheduler.unix_endpoint_count() || scheduler.open_description_count() != baseline)
                return Status::fault("pathname UNIX listener/pending-child cycle leaked ownership");
            if (auto status = fixture.vfs.unlink("/tmp/cycle-point.sock"); !status) return status;
        }
        return {};
    }();
    const auto drained = fixture.scheduler.drain_all_resources();
    fixture.~Fixture();
    return !result ? result : drained;
}
} // namespace ok::tests
