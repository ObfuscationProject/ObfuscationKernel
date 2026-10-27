// Actual hosted legacy ext4 provider operations with controlled I/O handoffs.
// Private access observes state; no synthetic phase is used for unlink/destroy.
#include "ext4_realtime_model.hpp"
#include "ok/fs/ext4/layout.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace ok;
using namespace ok::fs;
using Bytes = std::vector<std::byte>;
#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "legacy owned:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); \
} } while (false)

namespace
{
usize handoffs{}, retirement_pauses{}, raw_tombstones{}, deferred_drains{}, cleanup_errors{};
constexpr InitialNodeAttributes regular{NodeType::regular, 0644, default_uid, default_gid};
std::span<const std::byte> text(const char *value)
{ return {reinterpret_cast<const std::byte *>(value), std::strlen(value)}; }

class ControlledDevice final : public driver::BlockDevice
{
  public:
    explicit ControlledDevice(const Bytes &input) : bytes(input) {}
    driver::BlockGeometry geometry() const override
    { unlocked(); return {bytes.size() / 512, 512, true}; }
    Status read_blocks(u64 first, std::span<std::byte> output) override
    {
        if (auto status = callback('r'); !status) return status;
        CHECK(first * 512 + output.size() <= bytes.size());
        std::memcpy(output.data(), bytes.data() + first * 512, output.size()); return {};
    }
    Status write_blocks(u64 first, std::span<const std::byte> input) override
    {
        if (auto status = callback('w'); !status) return status;
        CHECK(first * 512 + input.size() <= bytes.size());
        std::memcpy(bytes.data() + first * 512, input.data(), input.size()); return {};
    }
    Status flush() override { return callback('f'); }
    void pause_next(char kind)
    {
        std::lock_guard guard(lock); CHECK(!pause && !paused);
        pause = kind; resumed = false;
    }
    void wait_paused()
    {
        std::unique_lock guard(lock);
        CHECK(changed.wait_for(guard, std::chrono::seconds(10), [&] { return paused; }));
    }
    void resume()
    { std::lock_guard guard(lock); CHECK(paused); resumed = true; changed.notify_all(); }
    void fail_next(char kind) { std::lock_guard guard(lock); failure = kind; }
    usize callbacks() { std::lock_guard guard(lock); return calls; }
    usize mutations() { std::lock_guard guard(lock); return mutation_calls; }
    Ext4FileSystem *owner{};
    Bytes bytes;
  private:
    void unlocked() const
    {
        if (owner) { CHECK(owner->identity_lock_.try_lock()); owner->identity_lock_.unlock(); }
    }
    Status callback(char kind)
    {
        unlocked();
        std::unique_lock guard(lock); ++calls;
        if (kind != 'r') ++mutation_calls;
        if (pause == kind)
        {
            pause = 0; paused = true; changed.notify_all();
            CHECK(changed.wait_for(guard, std::chrono::seconds(10), [&] { return resumed; }));
            paused = false;
        }
        if (failure == kind) { failure = 0; return Status::io_error("controlled cleanup failure"); }
        return {};
    }
    std::mutex lock;
    std::condition_variable changed;
    usize calls{}, mutation_calls{};
    char pause{}, failure{};
    bool paused{}, resumed{};
};
struct Fixture
{
    explicit Fixture(const Bytes &bytes) : disk(bytes), storage(std::make_unique<Ext4FileSystem>())
    { CHECK(fs().mount(disk)); disk.owner = storage.get(); CHECK(fs().root()); }
    Ext4FileSystem &fs() { return *storage; }
    Ext4Node &root() { return static_cast<Ext4Node &>(*fs().root()); }
    Ext4Node *create(std::string_view name)
    {
        auto result = root().create_child_retained(name, regular); CHECK(result);
        return static_cast<Ext4Node *>(result.value());
    }
    ControlledDevice disk;
    std::unique_ptr<Ext4FileSystem> storage;
};
void lock_available(Ext4FileSystem &fs)
{ CHECK(fs.identity_lock_.try_lock()); fs.identity_lock_.unlock(); }
void no_io_stale(ControlledDevice &disk, Ext4Node &node)
{
    const auto before = disk.callbacks();
    CHECK(!node.query_metadata()); CHECK(!node.read(0, 1)); CHECK(!node.write(0, text("stale")));
    CHECK(!node.truncate(0)); CHECK(!node.sync()); CHECK(!node.lookup_child("child"));
    CHECK(!node.lookup_child_retained("child")); CHECK(!node.create_child("child", regular));
    CHECK(!node.create_child_retained("child", regular)); CHECK(!node.create_symlink("child", "target"));
    CHECK(!node.remove("child", false)); CHECK(!node.chmod(0600)); CHECK(!node.chown(1, 1));
    CHECK(!node.list()); CHECK(!node.directory_path(node)); CHECK(!node.link("child", node));
    CHECK(!node.rename("child", node, "new")); CHECK(!node.retain()); CHECK(!node.release());
    CHECK(!node.expose_borrowed_identity());
    CHECK(disk.callbacks() == before);
}
void scoped_handoffs_and_destroy(const Bytes &input)
{
    Fixture fixture(input); auto &fs = fixture.fs(); auto &root = fixture.root();
    CHECK(root.create("lookup-owned", NodeType::regular));
    Result<Node *> found{Status::not_found()};
    fixture.disk.pause_next('r');
    std::thread lookup([&] { found = root.lookup_child_retained("lookup-owned"); });
    fixture.disk.wait_paused(); lock_available(fs);
    CHECK(root.remove("lookup-owned", false).code() == StatusCode::busy);
    CHECK(root.create("competing", NodeType::regular).code() == StatusCode::busy);
    fixture.disk.resume(); lookup.join(); CHECK(found); ++handoffs;
    auto *held = static_cast<Ext4Node *>(found.value());
    CHECK(held->references_ == 1 && !held->borrowed_identity_exposed_);
    CHECK(held->write(0, text("held data"))); CHECK(root.remove("lookup-owned", false));
    CHECK(held->phase_ == Ext4Node::Phase::detached && held->references_ == 1);
    const auto before = fixture.disk.callbacks();
    CHECK(fs.begin_operation(nullptr)); CHECK(fs.destroy(held->ino_).code() == StatusCode::busy);
    CHECK(fs.destroy(2).code() == StatusCode::busy); CHECK(fs.finish_operation());
    CHECK(fixture.disk.callbacks() == before);
    CHECK(held->retain() && held->references_ == 2); CHECK(held->release());
    auto data = held->read(0, 32); CHECK(data && data.value().size == 9);
    Status released;
    fixture.disk.pause_next('r');
    std::thread destroy([&] { released = held->release(); });
    fixture.disk.wait_paused(); lock_available(fs);
    CHECK(held->phase_ == Ext4Node::Phase::retiring && !held->references_);
    CHECK(!held->retain() && !held->expose_borrowed_identity());
    CHECK(root.query_metadata().status().code() == StatusCode::busy);
    fixture.disk.resume(); destroy.join(); CHECK(released); ++retirement_pauses;
    CHECK(held->phase_ == Ext4Node::Phase::vacant);
    auto *reused = fixture.create("reused"); CHECK(reused == held); CHECK(reused->release());

    Result<Node *> created{Status::not_found()};
    fixture.disk.pause_next('w');
    std::thread create([&] { created = root.create_child_retained("created-owned", regular); });
    fixture.disk.wait_paused(); lock_available(fs);
    CHECK(root.remove("created-owned", false).code() == StatusCode::busy);
    CHECK(root.lookup_child_retained("created-owned").status().code() == StatusCode::busy);
    fixture.disk.resume(); create.join(); CHECK(created); ++handoffs;
    auto *new_node = static_cast<Ext4Node *>(created.value());
    CHECK(new_node->references_ == 1 && !new_node->borrowed_identity_exposed_);
    CHECK(root.remove("created-owned", false)); CHECK(new_node->query_metadata()); CHECK(new_node->release());
    CHECK(fs.sync());
}
void raw_compatibility(const Bytes &input)
{
    for (int kind = 0; kind < 3; ++kind)
    {
        Fixture fixture(input); auto &root = fixture.root();
        Ext4Node *raw{};
        if (kind == 0)
        { CHECK(root.create("raw", NodeType::regular)); raw = static_cast<Ext4Node *>(root.lookup("raw")); }
        else if (kind == 1)
        { auto result = root.create_child("raw", regular); CHECK(result); raw = static_cast<Ext4Node *>(result.value()); }
        else
        {
            CHECK(root.create("raw", NodeType::regular));
            auto vfs = std::make_unique<VirtualFileSystem>(); CHECK(vfs->mount("/", root));
            raw = static_cast<Ext4Node *>(vfs->lookup("/raw"));
        }
        CHECK(raw && raw->borrowed_identity_exposed_ && !raw->references_);
        const auto old_ino = raw->ino_; const std::string old_name(raw->name());
        CHECK(raw->write(0, text("old raw data"))); CHECK(root.remove("raw", false));
        CHECK(raw->phase_ == Ext4Node::Phase::tombstone && raw->ino_ == old_ino && raw->name() == old_name);
        no_io_stale(fixture.disk, *raw);
        auto *replacement = fixture.create("replacement");
        CHECK(replacement != raw && replacement->ino_ == old_ino);
        CHECK(raw->ino_ == old_ino && raw->name() == old_name); no_io_stale(fixture.disk, *raw);
        CHECK(replacement->release());
        const auto calls = fixture.disk.callbacks();
        CHECK(fixture.fs().mount(fixture.disk).code() == StatusCode::busy);
        CHECK(fixture.disk.callbacks() == calls); ++raw_tombstones;
    }
}
void counted_threads(const Bytes &input)
{
    Fixture fixture(input); auto *node = fixture.create("counted");
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 8; ++index)
        workers.emplace_back([&] { for (unsigned step = 0; step < 10000; ++step) CHECK(node->retain() && node->release()); });
    for (auto &worker : workers) worker.join();
    CHECK(node->references_ == 1);
    { smp::ScopedSpinLock guard(fixture.fs().identity_lock_); node->references_ = ~u32{0}; }
    CHECK(node->retain().code() == StatusCode::overflow && node->references_ == ~u32{0});
    { smp::ScopedSpinLock guard(fixture.fs().identity_lock_); node->references_ = 1; }
    CHECK(node->release()); CHECK(node->release().code() == StatusCode::invalid_argument);
    CHECK(fixture.root().remove("counted", false)); CHECK(node->phase_ == Ext4Node::Phase::vacant);
}
void deferred_release(const Bytes &input, bool fail, bool retained_result)
{
    Fixture fixture(input); auto *victim = fixture.create("deferred");
    CHECK(victim->write(0, text("deferred payload"))); CHECK(fixture.root().remove("deferred", false));
    Ext4Node *lookup_target{};
    if (retained_result) { lookup_target = fixture.create("handoff"); CHECK(lookup_target->release()); }
    Result<Metadata> metadata{Status::not_found()}; Result<Node *> found{Status::not_found()};
    fixture.disk.pause_next('r');
    std::thread operation([&] {
        if (retained_result) found = fixture.root().lookup_child_retained("handoff");
        else metadata = fixture.root().query_metadata();
    });
    fixture.disk.wait_paused(); lock_available(fixture.fs());
    CHECK(victim->release()); CHECK(!victim->references_ && victim->phase_ == Ext4Node::Phase::detached);
    CHECK(!victim->retain() && !victim->expose_borrowed_identity());
    if (fail) fixture.disk.fail_next('w');
    fixture.disk.resume(); operation.join();
    if (fail)
    {
        CHECK((retained_result ? found.status() : metadata.status()).code() == StatusCode::io_error);
        CHECK(fixture.fs().failed_ && victim->phase_ == Ext4Node::Phase::retiring);
        CHECK(!victim->references_ && !fixture.fs().operation_active_);
        if (retained_result) CHECK(!lookup_target->references_);
        const auto mutations = fixture.disk.mutations();
        CHECK(!fixture.root().create("quarantined", NodeType::regular)); CHECK(!fixture.fs().sync());
        CHECK(fixture.fs().mount(fixture.disk).code() == StatusCode::busy);
        CHECK(fixture.disk.mutations() == mutations); ++cleanup_errors;
    }
    else
    { CHECK(metadata && victim->phase_ == Ext4Node::Phase::vacant && !fixture.fs().operation_active_); ++deferred_drains; }
}
void direct_release_failure(const Bytes &input)
{
    Fixture fixture(input); auto *node = fixture.create("cleanup-fail");
    CHECK(node->write(0, text("payload"))); CHECK(fixture.root().remove("cleanup-fail", false));
    fixture.disk.fail_next('w'); CHECK(node->release().code() == StatusCode::io_error);
    CHECK(!node->references_ && node->phase_ == Ext4Node::Phase::retiring && fixture.fs().failed_);
    CHECK(node->release().code() == StatusCode::invalid_argument);
    const auto mutations = fixture.disk.mutations(); CHECK(!node->retain()); CHECK(!node->read(0, 1));
    CHECK(!fixture.root().create("after-fail", NodeType::regular)); CHECK(!fixture.fs().sync());
    CHECK(fixture.disk.mutations() == mutations); ++cleanup_errors;
}
}
int main(int argc, char **argv)
{
    CHECK(argc == 2); testing::ext4_clock::reset();
    auto *file = std::fopen(argv[1], "rb"); CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END)); Bytes input(static_cast<usize>(std::ftell(file)));
    CHECK(!std::fseek(file, 0, SEEK_SET)); CHECK(std::fread(input.data(), 1, input.size(), file) == input.size());
    std::fclose(file);
    scoped_handoffs_and_destroy(input); raw_compatibility(input); counted_threads(input);
    deferred_release(input, false, false); deferred_release(input, true, false);
    deferred_release(input, true, true); direct_release_failure(input);
    CHECK(testing::ext4_clock::calls() == 0);
    std::printf("PASS actual legacy ext4 ownership: handoffs=%zu retirement_pauses=%zu raw_tombstones=%zu deferred_drains=%zu cleanup_errors=%zu counted_thread_pairs=80000 memory_lock_spans_io=0\n",
        handoffs, retirement_pauses, raw_tombstones, deferred_drains, cleanup_errors);
}
