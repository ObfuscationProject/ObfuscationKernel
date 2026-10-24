// Hosted provider-publication and RAM identity checks; no guest or SMP claim.
#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (false)

class LegacyNode : public fs::Node
{
  public:
    fs::Metadata attributes{.type = fs::NodeType::directory, .mode = fs::mode_directory | 02777,
                            .uid = 70, .gid = 0x87654321};
    usize old_creates{}, mode_changes{}, owner_changes{};
    std::string_view name() const override { return "provider"; }
    fs::Metadata metadata() const override { return attributes; }
    fs::Node *lookup(std::string_view child) override { return child == ".." ? this : nullptr; }
    Result<fs::FileBuffer> read(usize, usize) const override { return Status::unsupported(); }
    Status write(usize, std::span<const std::byte>) override { return Status::unsupported(); }
    Status create(std::string_view, fs::NodeType) override { ++old_creates; return Status::success(); }
    Status chmod(u32) override { ++mode_changes; return Status::io_error("late chmod must not run"); }
    Status chown(u32, u32) override { ++owner_changes; return Status::io_error("late chown must not run"); }
};

class PublishingNode final : public LegacyNode
{
  public:
    LegacyNode child;
    mutable usize queries{};
    usize fail_query{}, lookups{}, creates{};
    bool fail_lookup{}, fail_admission{}, fail_publication{}, null_success{}, published{};
    Result<fs::Metadata> query_metadata() const override
    {
        if (++queries == fail_query) return Status::io_error("initial parent metadata failed");
        return attributes;
    }
    Result<fs::Node *> lookup_child(std::string_view name) override
    {
        ++lookups;
        // Publication must return the node itself, without a second lookup.
        CHECK(!published);
        if (name == "new" && fail_lookup) return Status::io_error("initial lookup failed");
        return LegacyNode::lookup_child(name);
    }
    Result<fs::Node *> create_child(std::string_view name, const fs::InitialNodeAttributes &initial) override
    {
        CHECK(name == "new" && !published);
        ++creates;
        if (fail_admission) return Status::unsupported("initial attributes unsupported");
        if (fail_publication) return Status::no_space("publication capacity exhausted");
        if (null_success) return static_cast<fs::Node *>(nullptr);
        child.attributes = {.type = initial.type, .mode = fs::mode_for(initial.type, initial.mode),
                            .uid = initial.uid, .gid = initial.gid};
        // The first externally visible state already contains all attributes.
        CHECK(child.attributes.uid == 0xf1234567 && child.attributes.gid == attributes.gid);
        CHECK(child.attributes.mode == (fs::mode_directory | 03750));
        published = true;
        return static_cast<fs::Node *>(&child);
    }
};

void providers()
{
    {
        auto vfs = std::make_unique<fs::VirtualFileSystem>();
        LegacyNode legacy;
        CHECK(vfs->mount("/", legacy));
        auto status = vfs->create({}, "/new", fs::NodeType::directory, 01750, 0xf1234567, 18);
        CHECK(status.code() == StatusCode::unsupported && !legacy.old_creates);
        CHECK(!legacy.mode_changes && !legacy.owner_changes);
    }
    for (int failure = 0; failure < 7; ++failure)
    {
        auto vfs = std::make_unique<fs::VirtualFileSystem>();
        PublishingNode parent;
        CHECK(vfs->mount("/", parent));
        parent.queries = 0;
        if (failure == 1) parent.fail_query = 1;
        if (failure == 2) parent.fail_lookup = true;
        if (failure == 3) parent.fail_admission = true;
        if (failure == 4) parent.fail_publication = true;
        if (failure == 5) parent.attributes.link_count = 0;
        if (failure == 6) parent.null_success = true;
        auto status = vfs->create({}, "/new", fs::NodeType::directory, 01750, 0xf1234567, 18);
        CHECK(status.ok() == (failure == 0) && parent.published == (failure == 0));
        CHECK(parent.creates == ((failure == 0 || failure == 3 || failure == 4 || failure == 6) ? 1u : 0u));
        CHECK(!parent.old_creates && !parent.child.mode_changes && !parent.child.owner_changes);
        if (failure == 1 || failure == 2) CHECK(status.code() == StatusCode::io_error);
        if (failure == 5) CHECK(status.code() == StatusCode::not_found);
        if (failure == 6) CHECK(status.code() == StatusCode::io_error);
    }
}

void ram_inheritance()
{
    auto vfs = std::make_unique<fs::VirtualFileSystem>();
    CHECK(vfs->create("/group", fs::NodeType::directory));
    CHECK(vfs->chmod("/group", 02777));
    CHECK(vfs->chown("/group", 70, 0x87654321));
    CHECK(vfs->create({}, "/group/d", fs::NodeType::directory, 01750, 0xf1234567, 18));
    CHECK(vfs->create({}, "/group/f", fs::NodeType::regular, 0640, 0xf1234567, 18));
    CHECK(vfs->create({}, "/group/s", fs::NodeType::symlink, 0777, 0xf1234567, 18, "f"));
    auto directory = vfs->stat("/group/d");
    auto file = vfs->stat("/group/f");
    auto link = vfs->lookup("/group")->lookup_child("s");
    CHECK(directory && file && link);
    CHECK(directory.value().mode == (fs::mode_directory | 03750));
    CHECK(file.value().mode == (fs::mode_regular | 0640));
    CHECK(directory.value().uid == 0xf1234567 && directory.value().gid == 0x87654321);
    CHECK(file.value().uid == 0xf1234567 && file.value().gid == 0x87654321);
    CHECK(link.value()->metadata().gid == 0x87654321 && link.value()->metadata().mode == (fs::mode_symlink | 0777));
    CHECK(vfs->create({}, "/group/d/sub", fs::NodeType::directory, 0700, 99, 18));
    CHECK(vfs->stat("/group/d/sub").value().gid == 0x87654321);
    CHECK(vfs->stat("/group/d/sub").value().mode == (fs::mode_directory | 02700));
    CHECK(vfs->create({}, "/tmp/plain", fs::NodeType::directory, 01750, 0xf1234567, 18));
    CHECK(vfs->stat("/tmp/plain").value().mode == (fs::mode_directory | 01750));
    CHECK(vfs->stat("/tmp/plain").value().gid == 18);
}

void retained_parent_and_mask()
{
    auto vfs = std::make_unique<fs::VirtualFileSystem>();
    auto scheduler = std::make_unique<sched::Scheduler>();
    driver::ConsoleDriver console;
    posix::PosixService service;
    CHECK(console.start() && service.initialize(*vfs, console, *scheduler));
    auto process = scheduler->create_user_process("initial-child", arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
    CHECK(process && service.activate(process.value()));
    CHECK(service.mkdir("/original", 0777));
    CHECK(service.chmod("/original", 02777) && service.chown("/original", 0, 0x87654321));
    auto fd = service.open("/original", posix::o_RDONLY); CHECK(fd);
    CHECK(service.rename("/original", "/renamed"));
    CHECK(service.set_identity(0xf1234567, 18));
    CHECK(service.umask(0027) == 0022);
    CHECK(service.mkdirat(fd.value(), "child", 01777));
    auto child = service.stat("/renamed/child"); CHECK(child);
    CHECK(child.value().mode == (fs::mode_directory | 03750));
    CHECK(child.value().uid == 0xf1234567 && child.value().gid == 0x87654321);
    CHECK(service.mkdirat(fd.value(), "child", 0700).code() == StatusCode::already_exists);
    CHECK(service.close(fd.value()));
    CHECK(service.set_identity(0, 0));
    CHECK(service.chmod("/renamed", 02000));
    CHECK(service.set_identity(91, 92));
    CHECK(service.mkdir("/renamed/denied", 0777).code() == StatusCode::denied);
    CHECK(!vfs->lookup("/renamed/denied"));
}

int main()
{
    providers();
    ram_inheritance();
    retained_parent_and_mask();
    std::puts("PASS initial child hosted models: final metadata publication, fail-before-write provider, setgid inheritance, retained parent and process mask");
}
