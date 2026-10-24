// Real ext4 transport callbacks challenge VFS operation admission and owned release.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4_fs.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace ok;
using namespace ok::fs;
using Bytes = std::vector<std::byte>;

#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "hostile VFS/ext4 admission:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
class Disk final : public driver::BlockDevice
{
  public:
    explicit Disk(const Bytes &input) : bytes(input) {}
    driver::BlockGeometry geometry() const override
    { probe(); return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    {
        if (auto status = callback('r'); !status) return status;
        CHECK(first * 512 + output.size() <= bytes.size());
        std::memcpy(output.data(), bytes.data() + first * 512, output.size());
        return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        if (auto status = callback('w'); !status) return status;
        CHECK(first * 512 + input.size() <= bytes.size());
        std::memcpy(bytes.data() + first * 512, input.data(), input.size());
        return {};
    }
    Status flush() override { return callback('f'); }

    void pause_next(char kind)
    { std::lock_guard guard(lock); CHECK(!pause && !paused); pause = kind; resumed = false; }
    void wait_paused()
    {
        std::unique_lock guard(lock);
        CHECK(changed.wait_for(guard, std::chrono::seconds(10), [&] { return paused; }));
    }
    void resume()
    { std::lock_guard guard(lock); CHECK(paused); resumed = true; changed.notify_all(); }
    void fail_next(char kind)
    { std::lock_guard guard(lock); failure = kind; }

    std::atomic<usize> callbacks{}, reentrant_busy{}, unlocked_callbacks{};
    VirtualFileSystem *vfs{};
    Ext4FileSystem *owner{};
    bool probe_active{};
    Bytes bytes;
  private:
    void probe() const
    {
        if (owner)
        {
            CHECK(owner->identity_lock_.try_lock());
            owner->identity_lock_.unlock();
        }
        if (vfs)
        {
            CHECK(vfs->lock_.try_lock());
            const bool active = vfs->operation_active_;
            vfs->lock_.unlock();
            if (probe_active)
            {
                CHECK(active);
                ++const_cast<Disk *>(this)->unlocked_callbacks;
                const auto nested = vfs->stat("/victim");
                CHECK(!nested && nested.status().code() == StatusCode::busy);
                ++const_cast<Disk *>(this)->reentrant_busy;
            }
        }
    }
    Status callback(char kind)
    {
        probe();
        ++callbacks;
        std::unique_lock guard(lock);
        if (pause == kind)
        {
            pause = 0; paused = true; changed.notify_all();
            CHECK(changed.wait_for(guard, std::chrono::seconds(10), [&] { return resumed; }));
            paused = false;
        }
        if (failure == kind)
        {
            failure = 0;
            return Status::io_error("injected deferred ext4 cleanup failure");
        }
        return {};
    }
    mutable std::mutex lock;
    std::condition_variable changed;
    char pause{}, failure{};
    bool paused{}, resumed{};
};

struct Fixture
{
    explicit Fixture(const Bytes &input) : disk(input), storage(std::make_unique<Ext4FileSystem>()),
                                           vfs(std::make_unique<VirtualFileSystem>())
    {
        CHECK(storage->mount(disk)); disk.owner = storage.get();
        root = storage->root(); CHECK(root);
        CHECK(vfs->mount("/", *root)); disk.vfs = vfs.get();
        CHECK(root->create("victim", NodeType::regular));
        CHECK(root->create("orphan", NodeType::regular));
    }
    Disk disk;
    std::unique_ptr<Ext4FileSystem> storage;
    std::unique_ptr<VirtualFileSystem> vfs;
    Node *root{};
};

void deferred_release_during_vfs_lookup(const Bytes &input, bool fail_cleanup)
{
    Fixture fixture(input);
    auto held = fixture.vfs->resolve({}, "/orphan"); CHECK(held);
    auto *node = static_cast<Ext4Node *>(held.value().node());
    CHECK(node && node->references_ == 1);
    CHECK(fixture.root->remove("orphan", false));
    CHECK(node->phase_ == Ext4Node::Phase::detached && node->references_ == 1);

    fixture.disk.probe_active = true;
    fixture.disk.pause_next('r');
    Result<Metadata> result{Status::not_found()};
    std::thread worker([&] { result = fixture.vfs->stat("/victim"); });
    fixture.disk.wait_paused();
    CHECK(fixture.vfs->lock_.try_lock());
    CHECK(fixture.vfs->operation_active_);
    fixture.vfs->lock_.unlock();
    const auto callbacks = fixture.disk.callbacks.load();
    CHECK(held.value().release());
    CHECK(node->phase_ == Ext4Node::Phase::detached && !node->references_);
    CHECK(fixture.disk.callbacks.load() == callbacks);

    CHECK(fixture.vfs->stat("/victim").status().code() == StatusCode::busy);
    CHECK(fixture.vfs->resolve({}, "/victim").status().code() == StatusCode::busy);
    CHECK(fixture.vfs->create("/blocked", NodeType::regular).code() == StatusCode::busy);
    CHECK(fixture.vfs->unlink("/victim").code() == StatusCode::busy);
    CHECK(fixture.vfs->mount("/", *fixture.root).code() == StatusCode::busy);
    CHECK(fixture.disk.callbacks.load() == callbacks);

    sched::WaitRegistration registration;
    auto ticket = registration.arm(11, 13); CHECK(ticket);
    auto queued = fixture.vfs->queue_operation_wait(ticket.value());
    CHECK(queued && queued.value());
    if (fail_cleanup) fixture.disk.fail_next('w');
    fixture.disk.resume(); worker.join();
    CHECK(!fixture.vfs->operation_active_);
    auto ready = fixture.vfs->take_ready_operation_wait();
    CHECK(ready && ready.value() == ticket.value());
    CHECK(registration.complete(ready.value(), sched::WaitCompletion::ready));
    CHECK(!fixture.vfs->take_ready_operation_wait());
    CHECK(fixture.disk.reentrant_busy.load() >= 1);
    CHECK(fixture.disk.reentrant_busy.load() == fixture.disk.unlocked_callbacks.load());
    if (fail_cleanup)
    {
        CHECK(!result && result.status().code() == StatusCode::io_error);
        CHECK(node->phase_ == Ext4Node::Phase::retiring && fixture.storage->failed_);
    }
    else
    {
        CHECK(result && result.value().type == NodeType::regular);
        CHECK(node->phase_ == Ext4Node::Phase::vacant && !fixture.storage->failed_);
        CHECK(fixture.vfs->stat("/victim"));
    }
}
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    testing::ext4_clock::reset();
    auto *file = std::fopen(argv[1], "rb"); CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END));
    Bytes input(static_cast<usize>(std::ftell(file)));
    CHECK(!std::fseek(file, 0, SEEK_SET));
    CHECK(std::fread(input.data(), 1, input.size(), file) == input.size());
    std::fclose(file);
    deferred_release_during_vfs_lookup(input, false);
    deferred_release_during_vfs_lookup(input, true);
    CHECK(testing::ext4_clock::calls() == 0);
    std::puts("PASS actual VFS/ext4 admission: contending calls busy, transport callbacks unlocked and reentrant, owned release deferred, ready ticket, cleanup failure propagated");
}
