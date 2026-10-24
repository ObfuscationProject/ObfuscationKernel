#include <ok/fs/metadata_buffers.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::block;
using namespace ok::fs;

namespace
{
using Cache = MetadataBuffers<2, 1024, 2>;
using Session = Cache::Registry::Session;

usize checks{};
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "metadata Session invalidation:%d: %s\n", __LINE__, #__VA_ARGS__); \
    std::abort(); \
} } while (false)

struct Disk final : driver::BlockDevice
{
    usize reads{}, writes{}, flushes{};
    driver::BlockGeometry geometry() const override { return {64, 512, true}; }
    Status read_blocks(u64, std::span<std::byte> out) override
    {
        ++reads;
        for (auto &value : out) value = std::byte{0x5a};
        return {};
    }
    Status write_blocks(u64, std::span<const std::byte>) override
    { ++writes; return Status::denied("unexpected metadata write"); }
    Status flush() override
    { ++flushes; return Status::denied("unexpected metadata flush"); }
};

struct CaptureGuard
{
    Session *session{};
    Status begin_read(Session &value, u64, usize)
    { session = &value; return {}; }
    Status finish_read(Session &, Status result) { return result; }
};

struct RefusalGuard
{
    bool quarantine{};
    usize calls{};
    Status begin_read(Session &session, u64, usize)
    {
        ++calls;
        if (quarantine)
        {
            CHECK(session.abort(Status::fault("injected route integrity failure")));
            return Status::fault("injected route integrity failure");
        }
        return Status::overflow("injected route accounting refusal");
    }
    Status finish_read(Session &, Status result) { return result; }
};

void external_abort_denies_cached_data()
{
    Disk disk;
    Cache::Registry registry;
    const auto key = registry.register_device(disk, 101, 1024);
    CHECK(key);
    Cache cache;
    CHECK(cache.open(registry, key.value(), {0, 64}, SessionAccess::writable));
    CaptureGuard capture;
    Cache::Pin retained, hit;
    CHECK(cache.pin(1, retained, capture) && capture.session && disk.reads == 1);
    CHECK(capture.session->abort(Status::fault("external Session failure")));
    const auto session = registry.inspect(key.value());
    CHECK(session && session.value().state == DeviceState::quarantined);
    const auto owner = cache.report();
    CHECK(owner.state == MetadataOwnerState::failed && owner.first_error == StatusCode::fault);
    CHECK(cache.pin(1, hit).code() == StatusCode::denied && !hit.report());
    std::array<std::byte, 1> byte{};
    CHECK(retained.read(0, byte).code() == StatusCode::denied && disk.reads == 1);
    CHECK(retained.release());
    CHECK(!disk.writes && !disk.flushes);
}

void guard_quarantine_fails_owner_immediately()
{
    Disk disk;
    Cache::Registry registry;
    const auto key = registry.register_device(disk, 102, 1024);
    CHECK(key);
    Cache cache;
    CHECK(cache.open(registry, key.value(), {0, 64}, SessionAccess::writable));
    CaptureGuard capture;
    Cache::Pin retained, refused;
    CHECK(cache.pin(1, retained, capture));
    RefusalGuard guard{true};
    CHECK(cache.pin(2, refused, guard).code() == StatusCode::fault && guard.calls == 1);
    CHECK(cache.report().state == MetadataOwnerState::failed);
    const auto session = registry.inspect(key.value());
    CHECK(session && session.value().state == DeviceState::quarantined);
    std::array<std::byte, 1> byte{};
    CHECK(retained.read(0, byte).code() == StatusCode::denied);
    CHECK(cache.pin(1, refused).code() == StatusCode::denied && disk.reads == 1);
    CHECK(retained.release());
    CHECK(!disk.writes && !disk.flushes);
}

void accounting_refusal_retains_valid_session()
{
    Disk disk;
    Cache::Registry registry;
    const auto key = registry.register_device(disk, 103, 1024);
    CHECK(key);
    Cache cache;
    CHECK(cache.open(registry, key.value(), {0, 64}, SessionAccess::writable));
    CaptureGuard capture;
    Cache::Pin retained, refused, hit;
    CHECK(cache.pin(1, retained, capture));
    RefusalGuard guard{false};
    CHECK(cache.pin(2, refused, guard).code() == StatusCode::overflow && guard.calls == 1);
    CHECK(cache.report().state == MetadataOwnerState::ready);
    const auto session = registry.inspect(key.value());
    CHECK(session && session.value().state == DeviceState::owned);
    std::array<std::byte, 1> byte{};
    CHECK(retained.read(0, byte) && byte[0] == std::byte{0x5a});
    CHECK(cache.pin(1, hit) && hit.read(0, byte) && byte[0] == std::byte{0x5a});
    CHECK(disk.reads == 1);
    CHECK(hit.release() && retained.release());
    CHECK(cache.close() && registry.detach(key.value()));
    CHECK(!disk.writes && !disk.flushes);
}
} // namespace

int main()
{
    external_abort_denies_cached_data();
    guard_quarantine_fails_owner_immediately();
    accounting_refusal_retains_valid_session();
    std::printf("PASS hosted metadata Session invalidation checks=%zu native=0 writer=0\n", checks);
}
