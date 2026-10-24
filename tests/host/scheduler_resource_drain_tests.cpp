#include "ok/fs/vfs.hpp"
#include "ok/sched/scheduler.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace ok;
static usize checks{};
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!static_cast<bool>(condition))                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                          \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

// Public Node service boundary; failures can preserve unresolved provider holds.
class TestNode final : public fs::Node
{
  public:
    std::string_view name() const override { return "resource-node"; }
    fs::Metadata metadata() const override { return {}; }
    Result<fs::FileBuffer> read(usize, usize) const override
    { return Status::unsupported("resource drain model has no file media"); }
    Status write(usize, std::span<const std::byte>) override
    { return Status::unsupported("resource drain model has no file media"); }
    Node *lookup(std::string_view) override { return nullptr; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported("model creation"); }
    Status chmod(u32) override { return Status::unsupported("model chmod"); }
    Status chown(u32, u32) override { return Status::unsupported("model chown"); }
    Status retain() override
    {
        ++retains;
        ++references;
        return {};
    }
    Status release() override
    {
        ++releases;
        CHECK(references != 0);
        if (observe) observe(context);
        if (consume_release) --references;
        return failure;
    }
    usize retains{}, releases{}, references{};
    bool consume_release{true};
    Status failure{};
    void (*observe)(void *){};
    void *context{};
};

struct Fixture
{
    TestNode first, second, cwd;
    sched::Scheduler scheduler;
    u16 bind(sched::ProcessResources &resources, i32 fd, TestNode &node, usize mount)
    {
        const auto allocated = scheduler.allocate_description({.readable = true, .node = &node, .mount_id = mount});
        CHECK(allocated);
        CHECK(scheduler.install_descriptor(resources, fd, allocated.value()));
        return allocated.value();
    }
    void set_cwd(sched::ProcessResources &resources, usize mount)
    {
        CHECK(cwd.retain());
        resources.cwd_node = &cwd;
        resources.cwd_mount_id = mount;
    }
    sched::ProcessId process()
    {
        const auto created = scheduler.create_process("resource-drain", {});
        CHECK(created);
        return created.value();
    }
};

template <typename Test> void run_case(Test test)
{
    alignas(Fixture) static std::array<std::byte, sizeof(Fixture)> storage{};
    auto &fixture = *::new (storage.data()) Fixture;
    test(fixture);
    fixture.~Fixture();
}

void success_and_shared_fd()
{
    run_case([](Fixture &f) {
        auto &r = f.scheduler.boot_resources();
        const auto first = f.bind(r, 3, f.first, 37);
        CHECK(f.scheduler.install_descriptor(r, 4, first));
        const auto second = f.bind(r, 5, f.second, 38);
        f.set_cwd(r, 39);
        r.mappings[0].used = true;
        CHECK(f.first.retains == 1 && f.scheduler.open_description(first)->references == 2);
        CHECK(f.scheduler.drain_resources(r));
        CHECK(r.files.open_count() == 0 && !r.cwd_node && r.cwd_mount_id == 0 && !r.mappings[0].used);
        CHECK(!f.scheduler.open_description(first) && !f.scheduler.open_description(second));
        CHECK(f.first.releases == 1 && f.second.releases == 1 && f.cwd.releases == 1);
        CHECK(f.first.references == 0 && f.second.references == 0 && f.cwd.references == 0);
        const auto report = f.scheduler.resource_drain_report();
        CHECK(report.first_failure && report.descriptors_consumed == 6 && report.cwd_consumed == 1 && !report.active);
        CHECK(f.scheduler.drain_all_resources() && f.scheduler.resource_drain_report().descriptions_remaining == 0);
        CHECK(f.first.releases == 1 && f.cwd.releases == 1);
    });
}

void failures_consume_all_and_preserve_first()
{
    run_case([](Fixture &f) {
        auto &r = f.scheduler.boot_resources();
        const auto first = f.bind(r, 3, f.first, 71);
        const auto second = f.bind(r, 4, f.second, 72);
        f.set_cwd(r, 73);
        f.first.failure = Status::io_error("first provider cleanup failed");
        f.second.failure = Status::denied("second provider cleanup failed");
        f.cwd.failure = Status::fault("cwd provider cleanup failed");
        const auto status = f.scheduler.drain_resources(r);
        CHECK(status.code() == StatusCode::io_error && status.message() == f.first.failure.message());
        CHECK(r.files.open_count() == 0 && !r.cwd_node && r.cwd_mount_id == 0);
        CHECK(!f.scheduler.open_description(first) && !f.scheduler.open_description(second));
        CHECK(f.first.releases == 1 && f.second.releases == 1 && f.cwd.releases == 1);
        const auto report = f.scheduler.resource_drain_report();
        CHECK(report.first_failure.code() == StatusCode::io_error && report.failed_releases == 3);
        CHECK(report.first_failed_fd == 3 && report.first_failed_description == first && report.first_failed_mount_id == 71);
        CHECK(!report.first_failed_cwd && !report.active);
        CHECK(f.scheduler.drain_resources(r));
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::io_error);
        CHECK(f.first.releases == 1 && f.second.releases == 1 && f.cwd.releases == 1);
    });
}

void invalid_description_does_not_skip_other_owners()
{
    run_case([](Fixture &f) {
        auto &r = f.scheduler.boot_resources();
        *r.files.entry(3) = {.used = true, .description = sched::invalid_description};
        const auto second = f.bind(r, 4, f.second, 82);
        r.cwd_mount_id = 83;
        CHECK(f.scheduler.drain_resources(r).code() == StatusCode::bad_descriptor);
        CHECK(r.files.open_count() == 0 && !r.cwd_node && r.cwd_mount_id == 0);
        CHECK(f.second.releases == 1 && !f.scheduler.open_description(second));
        const auto report = f.scheduler.resource_drain_report();
        CHECK(report.first_failed_fd == 3 && report.first_failed_description == sched::invalid_description);
        CHECK(report.failed_releases == 1 && report.cwd_consumed == 0);
    });
}

void fork_shared_ofd_and_distinct_cwd()
{
    run_case([](Fixture &f) {
        const auto parent = f.process();
        auto &r = f.scheduler.find(parent)->resources();
        const auto description = f.bind(r, 3, f.first, 91);
        CHECK(f.scheduler.install_descriptor(r, 4, description));
        f.set_cwd(r, 92);
        const auto forked = f.scheduler.fork_process(parent);
        CHECK(forked);
        CHECK(f.scheduler.open_description(description)->references == 4);
        CHECK(f.first.retains == 1 && f.cwd.retains == 2);
        CHECK(f.scheduler.drain_resources(f.scheduler.find(forked.value())->resources()));
        CHECK(f.scheduler.open_description(description)->references == 2 && f.first.releases == 0);
        CHECK(f.cwd.releases == 1 && f.cwd.references == 1);
        CHECK(f.scheduler.drain_resources(r));
        CHECK(!f.scheduler.open_description(description) && f.first.releases == 1 && f.cwd.releases == 2);
        CHECK(f.scheduler.find(parent)->resources().cwd_mount_id == 0);
        CHECK(f.scheduler.find(forked.value())->resources().cwd_mount_id == 0);
        CHECK(f.scheduler.drain_all_resources());
    });
}

void exit_and_wait_keep_cleanup_separate()
{
    run_case([](Fixture &f) {
        const auto parent = f.process();
        const auto child = f.scheduler.fork_process(parent);
        CHECK(child);
        auto &r = f.scheduler.find(child.value())->resources();
        f.bind(r, 3, f.first, 101);
        f.set_cwd(r, 102);
        f.first.failure = Status::io_error("child filesystem cleanup failed");
        f.cwd.failure = Status::fault("child cwd cleanup failed");
        CHECK(f.scheduler.exit_process(child.value(), 29));
        CHECK(f.scheduler.find(child.value())->state() == sched::ProcessState::zombie);
        CHECK(f.scheduler.find(child.value())->wait_status() == (29 << 8));
        CHECK(r.files.open_count() == 0 && !r.cwd_node && r.cwd_mount_id == 0);
        const auto waited = f.scheduler.wait_process(parent, child.value());
        CHECK(waited && waited.value() == 29 && !f.scheduler.find(child.value()));
        CHECK(f.scheduler.resource_drain_report().first_failure.code() == StatusCode::io_error);
        CHECK(f.first.releases == 1 && f.cwd.releases == 1);
        const auto signaled = f.scheduler.fork_process(parent);
        CHECK(signaled && f.scheduler.terminate_process(signaled.value(), 15));
        CHECK(f.scheduler.find(signaled.value())->wait_status() == 15);
        CHECK(f.scheduler.wait_process(parent, signaled.value()).value() == 0);
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::io_error);
    });
}

void all_tasks_still_drain_after_boot_failure()
{
    run_case([](Fixture &f) {
        f.bind(f.scheduler.boot_resources(), 3, f.first, 111);
        const auto one = f.process(), two = f.process();
        auto &first = f.scheduler.find(one)->resources();
        auto &second = f.scheduler.find(two)->resources();
        f.bind(first, 3, f.second, 112);
        f.bind(second, 3, f.second, 113);
        f.set_cwd(first, 114);
        f.set_cwd(second, 115);
        const auto first_state = f.scheduler.find(one)->state(), second_state = f.scheduler.find(two)->state();
        f.first.failure = Status::io_error("boot cleanup failure");
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::io_error);
        CHECK(f.scheduler.boot_resources().files.open_count() == 0 && first.files.open_count() == 0 && second.files.open_count() == 0);
        CHECK(!first.cwd_node && !second.cwd_node && first.cwd_mount_id == 0 && second.cwd_mount_id == 0);
        CHECK(f.first.releases == 1 && f.second.releases == 2 && f.cwd.releases == 2);
        CHECK(f.scheduler.find(one)->state() == first_state && f.scheduler.find(two)->state() == second_state);
        CHECK(f.scheduler.resource_drain_report().descriptions_remaining == 0);
    });
}

void external_pins_and_reservations_remain_real()
{
    run_case([](Fixture &f) {
        auto &r = f.scheduler.boot_resources();
        const auto description = f.bind(r, 3, f.first, 121);
        CHECK(f.scheduler.retain_description(description));
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::busy);
        CHECK(r.files.open_count() == 0 && f.first.releases == 0 && f.first.references == 1);
        CHECK(f.scheduler.open_description(description)->references == 1);
        CHECK(f.scheduler.resource_drain_report().descriptions_remaining == 1);
        CHECK(f.scheduler.resource_drain_report().first_failure);
        CHECK(f.scheduler.release_description(description));
        CHECK(f.first.releases == 1 && f.scheduler.drain_all_resources());
        const auto reserved = f.scheduler.allocate_description({});
        CHECK(reserved);
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::busy);
        CHECK(f.scheduler.open_description(reserved.value())->references == 0);
        CHECK(f.scheduler.resource_drain_report().reservations_remaining == 1);
        CHECK(f.scheduler.retain_description(reserved.value()) && f.scheduler.release_description(reserved.value()));
        CHECK(f.scheduler.drain_all_resources());
    });
}

void failed_lease_evidence_and_callback_admission()
{
    run_case([](Fixture &f) {
        auto &r = f.scheduler.boot_resources();
        const auto description = f.bind(r, 3, f.first, 131);
        f.bind(r, 4, f.second, 132);
        f.set_cwd(r, 133);
        struct Context { Fixture &fixture; sched::ProcessResources &resources; u16 description; } context{f, r, description};
        f.first.context = &context;
        f.first.observe = [](void *value) {
            auto &c = *static_cast<Context *>(value);
            CHECK(!c.resources.files.contains(3) && !c.fixture.scheduler.open_description(c.description));
            CHECK(c.fixture.scheduler.resource_drain_report().active);
            CHECK(c.fixture.scheduler.retain_description(c.description).code() == StatusCode::bad_descriptor);
            CHECK(c.fixture.scheduler.drain_resources(c.resources).code() == StatusCode::busy);
            CHECK(c.fixture.scheduler.drain_all_resources().code() == StatusCode::busy);
        };
        f.cwd.context = &context;
        f.cwd.observe = [](void *value) {
            auto &c = *static_cast<Context *>(value);
            CHECK(!c.resources.cwd_node && c.resources.cwd_mount_id == 0);
        };
        f.first.consume_release = false;
        f.first.failure = Status::fault("corrupted provider lease remains unresolved");
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::fault);
        CHECK(f.first.references == 1 && f.first.releases == 1);
        CHECK(f.second.references == 0 && f.cwd.references == 0);
        CHECK(r.files.open_count() == 0 && !r.cwd_node && r.cwd_mount_id == 0);
        CHECK(f.scheduler.drain_all_resources().code() == StatusCode::fault);
        CHECK(f.first.references == 1 && f.first.releases == 1);
        CHECK(f.scheduler.resource_drain_report().failed_releases == 1 && !f.scheduler.resource_drain_report().active);
    });
}

int main()
{
    success_and_shared_fd();
    failures_consume_all_and_preserve_first();
    invalid_description_does_not_skip_other_owners();
    fork_shared_ofd_and_distinct_cwd();
    exit_and_wait_keep_cleanup_separate();
    all_tasks_still_drain_after_boot_failure();
    external_pins_and_reservations_remain_real();
    failed_lease_evidence_and_callback_admission();
    std::printf("PASS scheduler resource drain checks=%zu cases=8 consume_all=1 first_failure=1 shared_ofd=1 fork_cwd=1 exit_wait=1 all_tasks=1 external_pins=1 failed_provider_hold=1 callback_clear=1 reentry_refused=1 native_media=0\n", checks);
}
