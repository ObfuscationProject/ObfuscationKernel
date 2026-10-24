// Host provider-callback and wait-notification models; no native parking claim.
#include "ok/fs/vfs.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

using namespace ok;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (false)

namespace
{
usize paused_controls{}, reentrant_controls{}, ready_controls{}, callback_checks{}, early_returns{};
struct Monitor
{
    explicit Monitor(fs::VirtualFileSystem *vfs) : vfs(vfs) {}
    fs::VirtualFileSystem *vfs{};
    std::mutex mutex;
    std::condition_variable condition;
    char target{};
    bool paused{}, resume{}, used{}, reenter{true};
    std::atomic<usize> calls{};
    void check(char kind)
    {
        CHECK(vfs->lock_.try_lock());
        const bool active = vfs->operation_active_;
        vfs->lock_.unlock();
        ++calls;
        if (reenter && active)
        {
            const auto nested = vfs->stat("/file");
            CHECK(!nested && nested.status().code() == StatusCode::busy);
            CHECK(vfs->create("/nested", fs::NodeType::regular).code() == StatusCode::busy);
            vfs->set_mode(fs::FileSystemMode::ram_only);
            CHECK(vfs->mode() == fs::FileSystemMode::ram_only);
        }
        if (kind != target) return;
        std::unique_lock guard(mutex);
        if (used) return;
        used = true; paused = true; condition.notify_all();
        condition.wait(guard, [&] { return resume; });
    }
};

struct Provider final : fs::Node
{
    Monitor &monitor;
    fs::NodeType kind;
    Provider *child{}, *created{};
    usize holds{};
    bool published{}, metadata_error{}, release_error{};
    explicit Provider(Monitor &monitor, fs::NodeType kind) : monitor(monitor), kind(kind) {}
    std::string_view name() const override { monitor.check('n'); return "/"; }
    const void *filesystem_identity() const override { monitor.check('i'); return &monitor; }
    fs::Metadata metadata() const override
    { monitor.check('m'); return {.type = kind, .mode = fs::mode_for(kind, 0777), .link_count = 1}; }
    Result<fs::Metadata> query_metadata() const override
    { if (metadata_error) { monitor.check('m'); return Status::io_error("provider metadata refused"); } return metadata(); }
    Status retain() override { monitor.check('t'); ++holds; return {}; }
    Status release() override
    { monitor.check('r'); CHECK(holds); --holds; return release_error ? Status::io_error("provider release refused") : Status{}; }
    Status expose_borrowed_identity() override { monitor.check('e'); return {}; }
    fs::Node *lookup(std::string_view name) override
    { monitor.check('l'); return name == "file" ? child : name == "new" && published ? created : nullptr; }
    Result<fs::Node *> lookup_child_retained(std::string_view name) override
    {
        auto *found = lookup(name);
        if (!found) return Status::not_found("provider child absent");
        if (auto status = found->retain(); !status) return status;
        return found;
    }
    Result<fs::Node *> create_child_retained(std::string_view name, const fs::InitialNodeAttributes &) override
    { monitor.check('c'); CHECK(name == "new" && created); published = true; CHECK(created->retain()); return static_cast<fs::Node *>(created); }
    Result<fs::FileBuffer> read(usize, usize) const override { monitor.check('d'); return fs::FileBuffer{}; }
    Status write(usize, std::span<const std::byte>) override { monitor.check('w'); return {}; }
    Status truncate(usize) override { monitor.check('w'); return {}; }
    Status create(std::string_view, fs::NodeType) override { monitor.check('c'); return Status::unsupported(); }
    Status chmod(u32) override { monitor.check('a'); return {}; }
    Status chown(u32, u32) override { monitor.check('a'); return {}; }
    Result<fs::DirectoryListing> list(fs::DirectoryReadIntent) const override
    { monitor.check('s'); return fs::DirectoryListing{}; }
    Status remove(std::string_view, bool) override { monitor.check('x'); return {}; }
    Status link(std::string_view, fs::Node &) override { monitor.check('x'); return {}; }
    Status rename(std::string_view, fs::Node &, std::string_view) override { monitor.check('x'); return {}; }
    Result<FixedString<96>> directory_path(const fs::Node &) const override
    { monitor.check('p'); FixedString<96> path; CHECK(path.assign("/")); return path; }
};

struct Fixture
{
    std::unique_ptr<fs::VirtualFileSystem> vfs = std::make_unique<fs::VirtualFileSystem>();
    Monitor monitor{vfs.get()};
    Provider root{monitor, fs::NodeType::directory}, file{monitor, fs::NodeType::regular},
             created{monitor, fs::NodeType::regular};
    Fixture() { root.child = &file; root.created = &created; CHECK(vfs->mount("/", root)); }
    ~Fixture() { CHECK(!root.holds && !file.holds && !created.holds); callback_checks += monitor.calls; }
};

void check_busy(Fixture &f)
{
    const auto count = f.monitor.calls.load();
    CHECK(f.vfs->mount("/", f.root).code() == StatusCode::busy);
    CHECK(f.vfs->create("/busy", fs::NodeType::regular).code() == StatusCode::busy);
    CHECK(!f.vfs->create_file({}, "/new", 0644, 0, 0, false));
    CHECK(f.vfs->create_readonly_file("/archive", {}).code() == StatusCode::busy);
    CHECK(f.vfs->link("/file", "/link").code() == StatusCode::busy);
    CHECK(f.vfs->rename("/file", "/renamed").code() == StatusCode::busy);
    CHECK(f.vfs->unlink("/file").code() == StatusCode::busy);
    CHECK(f.vfs->rmdir("/file").code() == StatusCode::busy);
    CHECK(f.vfs->chmod("/file", 0644).code() == StatusCode::busy);
    CHECK(f.vfs->chown("/file", 1, 1).code() == StatusCode::busy);
    CHECK(f.vfs->write_file("/file", {}).code() == StatusCode::busy);
    CHECK(!f.vfs->read_file("/file") && !f.vfs->list("/") && !f.vfs->stat("/file"));
    CHECK(!f.vfs->query_metadata(f.file) && !f.vfs->directory_path(f.root));
    CHECK(!f.vfs->resolve({}, "/file") && !f.vfs->lookup("/file"));
    CHECK(f.monitor.calls.load() == count);
}

void paused_provider(char callback)
{
    Fixture f; f.monitor.target = callback;
    Status finished = Status::fault("worker did not finish");
    std::thread worker([&] {
        if (callback == 'c') finished = f.vfs->create("/new", fs::NodeType::regular);
        else { auto value = f.vfs->stat("/file"); finished = value ? Status{} : value.status(); }
    });
    {
        std::unique_lock guard(f.monitor.mutex);
        CHECK(f.monitor.condition.wait_for(guard, std::chrono::seconds(3), [&] { return f.monitor.paused; }));
    }
    CHECK(f.vfs->lock_.try_lock()); f.vfs->lock_.unlock(); check_busy(f);
    std::array<sched::WaitRegistration, fs::VirtualFileSystem::operation_wait_capacity> registrations;
    std::array<sched::WaitTicket, fs::VirtualFileSystem::operation_wait_capacity> tickets;
    for (usize index = 0; index < tickets.size(); ++index)
    {
        auto armed = registrations[index].arm(1, index + 1); CHECK(armed); tickets[index] = armed.value();
        auto queued = f.vfs->queue_operation_wait(tickets[index]); CHECK(queued && queued.value());
    }
    CHECK(f.vfs->queue_operation_wait(tickets[0]).status().code() == StatusCode::already_exists);
    auto stale = tickets[0]; ++stale.generation;
    CHECK(!f.vfs->cancel_operation_wait(stale));
    CHECK(f.vfs->cancel_operation_wait(tickets[0]));
    CHECK(f.vfs->queue_operation_wait(tickets[0]).value());
    auto full = f.vfs->queue_operation_wait({2, 99, 1}); CHECK(!full && full.status().code() == StatusCode::no_memory);
    CHECK(!f.vfs->take_ready_operation_wait());
    { std::lock_guard guard(f.monitor.mutex); f.monitor.resume = true; }
    f.monitor.condition.notify_all(); worker.join(); CHECK(finished);
    for (usize index = 0; index < tickets.size(); ++index)
    {
        auto ready = f.vfs->take_ready_operation_wait(); CHECK(ready);
        CHECK(ready.value().owner == 1 && ready.value().thread >= 1 && ready.value().thread <= tickets.size());
        auto &registration = registrations[ready.value().thread - 1];
        CHECK(registration.complete(ready.value(), sched::WaitCompletion::ready)); ++ready_controls;
        CHECK(!f.vfs->cancel_operation_wait(ready.value()));
    }
    CHECK(!f.vfs->take_ready_operation_wait());
    auto raced = f.vfs->queue_operation_wait({3, 99, 1}); CHECK(raced && !raced.value());
    CHECK(!f.vfs->queue_operation_wait({0, 99, 1}));
    CHECK(f.vfs->stat("/file")); ++paused_controls;
}

void early_and_reentrant()
{
    Fixture f;
    CHECK(f.vfs->chmod("/file", 0644)); CHECK(f.vfs->chown("/file", 1, 1));
    CHECK(f.vfs->write_file("/file", {})); CHECK(f.vfs->read_file("/file")); CHECK(f.vfs->list("/"));
    CHECK(f.vfs->query_metadata(f.file)); CHECK(f.vfs->directory_path(f.root));
    CHECK(f.vfs->link("/file", "/link")); CHECK(f.vfs->rename("/file", "/new-name"));
    CHECK(f.vfs->unlink("/file")); ++reentrant_controls;
    f.file.metadata_error = true;
    auto failed = f.vfs->stat("/file"); CHECK(!failed && failed.status().message() == "provider metadata refused");
    f.file.metadata_error = false; CHECK(f.vfs->stat("/file")); ++early_returns;
    f.file.release_error = true;
    failed = f.vfs->stat("/file"); CHECK(!failed && failed.status().message() == "provider release refused");
    f.file.release_error = false; CHECK(f.vfs->stat("/file")); ++early_returns;
    CHECK(!f.vfs->resolve({}, "/absent")); CHECK(f.vfs->stat("/file")); ++early_returns;
    f.vfs->operation_serial_ = ~u64{0};
    failed = f.vfs->stat("/file"); CHECK(!failed && failed.status().code() == StatusCode::overflow);
    CHECK(!f.vfs->operation_active_); CHECK(f.vfs->lock_.try_lock()); f.vfs->lock_.unlock(); ++early_returns;
}
}

int main()
{
    for (char kind : {'m', 'l', 'c', 'r'}) paused_provider(kind);
    early_and_reentrant();
    std::printf("PASS VFS admission callbacks: paused=%zu reentrant=%zu ready_tokens=%zu early_returns=%zu callback_lock_checks=%zu synchronous_busy_incomplete=1 native_parking=0\n",
                paused_controls, reentrant_controls, ready_controls, early_returns, callback_checks);
}
