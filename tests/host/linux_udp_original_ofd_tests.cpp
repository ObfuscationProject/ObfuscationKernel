#include "../../src/user/linux_udp_original_ofd.hpp"
#include "ok/net/native_udp.hpp"
#include "ok/fs/vfs.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

using namespace ok;
namespace udp = user::native::linux_udp;
static usize checks{};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "UDP_OFD_FAIL line=%d checks=%zu: %s\n", __LINE__, checks, #x); std::abort(); } } while (false)
static_assert(!std::is_copy_constructible_v<udp::OriginalUdpDescription>);
static_assert(!std::is_move_constructible_v<udp::OriginalUdpDescription>);
namespace ok::arch::execution { Status destroy_space(AddressSpace &space) { space = {}; return {}; } }

struct Fixture
{
    std::unique_ptr<sched::Scheduler> scheduler{std::make_unique<sched::Scheduler>()};
    net::NativeUdpService service;
    sched::ProcessId pid{};
    u16 description{};
    u32 endpoint{};
    Fixture()
    {
        auto created = scheduler->create_user_process("udp-ofd-host-model",
            arch::configured_arch_operations().make_user_context({arch::execution::user_begin,
                                                                   arch::execution::user_begin + 4096}));
        CHECK(created); pid = created.value();
        CHECK(service.configure({.local_mac = {2, 0, 0, 0, 0, 1}, .peer_mac = {2, 0, 0, 0, 0, 2},
                                 .local_ip = {10, 0, 2, 15}, .peer_ip = {10, 0, 2, 2}}));
        CHECK(scheduler->set_native_udp_service(service));
        auto opened = service.open(); CHECK(opened); endpoint = opened.value();
        auto allocated = scheduler->allocate_description({.readable = true, .writable = true,
            .flags = 2048, .native_udp_handle = endpoint}); CHECK(allocated); description = allocated.value();
        CHECK(scheduler->install_descriptor(process().resources(), 3, description));
    }
    ~Fixture()
    {
        if (process().fd_table().entry(3)->used) CHECK(scheduler->close_descriptor(process().resources(), 3));
    }
    sched::ProcessControlBlock &process() { return *scheduler->find(pid); }
};

void canonical_close_and_reuse()
{
    Fixture f; Status failure;
    u16 replacement{};
    {
        udp::OriginalUdpDescription hold{*f.scheduler, failure};
        auto selected = hold.acquire(f.process(), 3); CHECK(selected);
        CHECK(selected.value().kind == udp::DescriptionKind::retained_udp);
        CHECK(selected.value().endpoint == f.endpoint && selected.value().flags == 2048);
        CHECK(hold.held()); CHECK(f.scheduler->open_description(f.description)->references == 2);
        f.scheduler->open_description(f.description)->flags = 0;
        CHECK(selected.value().flags == 2048);
        CHECK(f.scheduler->close_descriptor(f.process().resources(), 3));
        CHECK(f.scheduler->open_description(f.description)->references == 1);
        CHECK(f.service.binding(f.endpoint));
        auto other = f.scheduler->allocate_description({.readable = true}); CHECK(other); replacement = other.value();
        CHECK(replacement != f.description);
        CHECK(f.scheduler->install_descriptor(f.process().resources(), 3, replacement));
        CHECK(hold.release()); CHECK(!hold.held()); CHECK(!f.service.binding(f.endpoint));
        CHECK(!f.scheduler->open_description(f.description));
        CHECK(hold.release()); CHECK(!hold.acquire(f.process(), 3));
        CHECK(f.scheduler->open_description(replacement)->references == 1);
        auto reused = f.scheduler->allocate_description({.readable = true}); CHECK(reused); CHECK(reused.value() == f.description);
        CHECK(f.scheduler->retain_description(reused.value()));
        CHECK(hold.release()); CHECK(f.scheduler->open_description(reused.value())->references == 1);
        CHECK(f.scheduler->release_description(reused.value()));
    }
    CHECK(failure); CHECK(f.scheduler->open_description(replacement)->references == 1);
}

void fallback_and_selection()
{
    Fixture f; Status failure;
    {
        udp::OriginalUdpDescription hold{*f.scheduler, failure};
        auto result = hold.acquire(f.process(), 3); CHECK(result);
        CHECK(f.scheduler->close_descriptor(f.process().resources(), 3));
    }
    CHECK(failure); CHECK(!f.service.binding(f.endpoint)); CHECK(!f.scheduler->open_description(f.description));
    for (i32 fd : {-1, 3, 31, 32, __INT32_MAX__})
    {
        udp::OriginalUdpDescription hold{*f.scheduler, failure};
        auto result = hold.acquire(f.process(), fd); CHECK(result);
        CHECK(result.value().kind == udp::DescriptionKind::bad_fd); CHECK(!hold.held()); CHECK(hold.release());
    }
    udp::OriginalUdpDescription console{*f.scheduler, failure};
    auto result = console.acquire(f.process(), 0); CHECK(result);
    CHECK(result.value().kind == udp::DescriptionKind::not_socket); CHECK(!console.held());
}

void failure_consumes_original_edge()
{
    Fixture f; Status failure;
    udp::OriginalUdpDescription hold{*f.scheduler, failure};
    CHECK(hold.acquire(f.process(), 3));
    CHECK(f.scheduler->close_descriptor(f.process().resources(), 3));
    // Simulate an independently retired provider to exercise fallible release.
    CHECK(f.service.close(f.endpoint));
    const auto first = hold.release(); CHECK(!first); CHECK(!failure); CHECK(!hold.held());
    CHECK(failure.code() == first.code()); CHECK(!f.scheduler->open_description(f.description));
    const auto saved = failure;
    auto reused = f.scheduler->allocate_description({.readable = true}); CHECK(reused); CHECK(reused.value() == f.description);
    CHECK(f.scheduler->retain_description(reused.value())); CHECK(hold.release());
    CHECK(failure.code() == saved.code()); CHECK(f.scheduler->open_description(reused.value())->references == 1);
    CHECK(f.scheduler->release_description(reused.value()));
}

struct ResetAccess
{
    Fixture &fixture;
    udp::OriginalUdpDescription &hold;
    usize resets{};
    bool recursive_release{};
    void reset()
    {
        ++resets; CHECK(fixture.service.binding(fixture.endpoint));
        if (recursive_release) CHECK(hold.release());
    }
};
void lifetime_order()
{
    for (bool reenter : {false, true})
    {
        Fixture f; Status failure; udp::OriginalUdpDescription hold{*f.scheduler, failure};
        CHECK(hold.acquire(f.process(), 3)); CHECK(f.scheduler->close_descriptor(f.process().resources(), 3));
        ResetAccess access{f, hold, 0, reenter};
        {
            udp::ImmediateLifetimeScope scope{access, hold};
            CHECK(scope.finish()); CHECK(access.resets == 1); CHECK(!hold.held()); CHECK(scope.finish());
        }
        CHECK(access.resets == 1); CHECK(!f.service.binding(f.endpoint)); CHECK(failure);
    }
}

// No callback reproductions: structural admission must reject these public OFD
// shapes before retaining them or invoking any node provider.
struct CountingNode final : fs::Node
{
    usize retains{}, releases{};
    std::string_view name() const override { return "shape-probe"; }
    fs::Metadata metadata() const override { return {}; }
    Result<fs::FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    Status retain() override { ++retains; return {}; }
    Status release() override { ++releases; return {}; }
    fs::Node *lookup(std::string_view) override { return nullptr; }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Status chmod(u32) override { return Status::unsupported(); }
    Status chown(u32, u32) override { return Status::unsupported(); }
};
void structural_admission()
{
    for (unsigned shape = 0; shape < 5; ++shape)
    {
        Fixture f; Status failure; CountingNode node;
        auto *open = f.scheduler->open_description(f.description); CHECK(open);
        const auto original = *open;
        if (shape == 0) open->node = &node;
        if (shape == 1) open->pipe = 0;
        if (shape == 2) open->console = true;
        if (shape == 3) open->input_console = true;
        if (shape == 4) open->references = 0;
        const auto selected_references = open->references;
        udp::OriginalUdpDescription hold{*f.scheduler, failure};
        auto selected = hold.acquire(f.process(), 3);
        CHECK(!selected && selected.status().code() == StatusCode::io_error);
        CHECK(!hold.held() && open->references == selected_references);
        CHECK(node.retains == 0 && node.releases == 0);
        CHECK(f.service.binding(f.endpoint));
        CHECK(hold.release() && failure); CHECK(!hold.acquire(f.process(), 3));
        *open = original; // Restore the owned fixture before its ordinary cleanup.
    }
    {
        Fixture canonical, foreign; Status failure;
        udp::OriginalUdpDescription hold{*canonical.scheduler, failure};
        const auto original = canonical.scheduler->open_description(canonical.description)->references;
        auto selected = hold.acquire(foreign.process(), 3);
        CHECK(!selected && selected.status().code() == StatusCode::denied);
        CHECK(!hold.held()); CHECK(hold.release() && failure);
        CHECK(canonical.scheduler->open_description(canonical.description)->references == original);
        CHECK(foreign.scheduler->open_description(foreign.description)->references == 1);
        CHECK(!hold.acquire(canonical.process(), 3));
    }
}

int main()
{
    canonical_close_and_reuse(); fallback_and_selection(); failure_consumes_original_edge(); lifetime_order(); structural_admission();
    std::printf("LINUX_UDP_ORIGINAL_OFD_MODEL_PASS checks=%zu hosted=1 canonical_scheduler=1 image_access=synthetic syscall=0 guest=0 nic=0\n", checks);
}
