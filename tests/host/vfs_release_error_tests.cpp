// Hosted service-boundary tests. These nodes consume each retain even when
// release reports a deferred I/O error; no storage reclamation is simulated.
#include "ok/fs/vfs.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ok;

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #condition); std::abort(); } } while (false)

struct CountedNode final : fs::Node
{
    explicit CountedNode(fs::NodeType kind, std::string_view edge) : kind(kind), edge(edge) {}
    fs::NodeType kind;
    std::string_view edge;
    std::string_view link_target{};
    CountedNode *parent{};
    std::array<CountedNode *, 8> children{};
    usize child_count{}, pins{}, retains{}, releases{}, fail_release_at{}, queries{}, fail_query_at{};
    usize mutations{};
    bool fail_mutation{}, published{};
    CountedNode *created{};
    std::string_view release_message{"cleanup failed"};

    void add(CountedNode &node)
    {
        CHECK(child_count < children.size());
        children[child_count++] = &node;
        node.parent = this;
    }
    void drained() const { CHECK(pins == 0 && retains == releases); }
    std::string_view name() const override { return edge; }
    fs::Metadata metadata() const override
    {
        return {.type = kind, .size = link_target.size(), .mode = fs::mode_for(kind, 0777), .link_count = 1};
    }
    Result<fs::Metadata> query_metadata() const override
    {
        auto *self = const_cast<CountedNode *>(this);
        if (++self->queries == fail_query_at) return Status::io_error("primary metadata failure");
        return metadata();
    }
    Result<fs::FileBuffer> read(usize, usize count) const override
    {
        fs::FileBuffer result{};
        result.size = count < link_target.size() ? count : link_target.size();
        for (usize i = 0; i < result.size; ++i) result.data[i] = static_cast<std::byte>(link_target[i]);
        return result;
    }
    Status write(usize, std::span<const std::byte>) override
    {
        ++mutations;
        return fail_mutation ? Status::no_space("primary mutation failure") : Status{};
    }
    Status truncate(usize) override
    {
        ++mutations;
        return fail_mutation ? Status::no_space("primary mutation failure") : Status{};
    }
    Status retain() override { ++pins; ++retains; return {}; }
    Status release() override
    {
        CHECK(pins);
        --pins;
        ++releases;
        return releases == fail_release_at ? Status::io_error(release_message) : Status{};
    }
    Status expose_borrowed_identity() override { return {}; }
    fs::Node *lookup(std::string_view child) override
    {
        if (child == ".") return this;
        if (child == "..") return parent ? parent : this;
        for (usize i = 0; i < child_count; ++i)
            if ((children[i] != created || published) && children[i]->edge == child) return children[i];
        return nullptr;
    }
    Status create(std::string_view, fs::NodeType) override { return Status::unsupported(); }
    Result<fs::Node *> create_child_retained(std::string_view name, const fs::InitialNodeAttributes &) override
    {
        ++mutations;
        if (fail_mutation) return Status::no_space("primary mutation failure");
        if (!created || created->edge != name) return Status::unsupported();
        published = true;
        if (auto status = created->retain(); !status) return status;
        return static_cast<fs::Node *>(created);
    }
    Status chmod(u32) override
    {
        ++mutations;
        return fail_mutation ? Status::no_space("primary mutation failure") : Status{};
    }
    Status chown(u32, u32) override
    {
        ++mutations;
        return fail_mutation ? Status::no_space("primary mutation failure") : Status{};
    }
    Status link(std::string_view, fs::Node &) override
    {
        ++mutations;
        return fail_mutation ? Status::no_space("primary mutation failure") : Status{};
    }
    Status rename(std::string_view, fs::Node &, std::string_view) override
    {
        ++mutations;
        return fail_mutation ? Status::no_space("primary mutation failure") : Status{};
    }
    Status remove(std::string_view, bool) override
    {
        ++mutations;
        return fail_mutation ? Status::no_space("primary mutation failure") : Status{};
    }
    Result<FixedString<96>> directory_path(const fs::Node &root) const override
    {
        FixedString<96> result;
        if (&root == this) static_cast<void>(result.assign("/"));
        else static_cast<void>(result.assign("/dir"));
        return result;
    }
};

struct Fixture
{
    std::unique_ptr<fs::VirtualFileSystem> vfs{std::make_unique<fs::VirtualFileSystem>()};
    CountedNode root{fs::NodeType::directory, "/"};
    CountedNode dir{fs::NodeType::directory, "dir"};
    CountedNode file{fs::NodeType::regular, "file"};
    CountedNode symlink{fs::NodeType::symlink, "link"};
    CountedNode created{fs::NodeType::regular, "new"};
    CountedNode mounted{fs::NodeType::directory, "mounted"};

    Fixture()
    {
        root.add(dir);
        root.add(file);
        root.add(symlink);
        dir.add(file);
        dir.add(created);
        dir.created = &created;
        symlink.link_target = "dir/file";
        CHECK(vfs->mount("/", root));
    }
    ~Fixture()
    {
        root.drained(); dir.drained(); file.drained(); symlink.drained(); created.drained(); mounted.drained();
    }
};

void resolver_handoffs()
{
    {
        Fixture f;
        f.dir.fail_release_at = 1;
        auto result = f.vfs->resolve({}, "/dir/file");
        CHECK(!result && result.status().message() == "cleanup failed");
        CHECK(f.dir.releases == 1 && f.file.releases == 1);
    }
    {
        Fixture f;
        f.dir.fail_release_at = 1;
        auto result = f.vfs->resolve({}, "/dir/../file");
        CHECK(!result && result.status().message() == "cleanup failed");
        CHECK(f.root.retains == f.root.releases);
    }
    {
        Fixture f;
        f.symlink.fail_release_at = 1;
        auto result = f.vfs->resolve({}, "/link");
        CHECK(!result && result.status().message() == "cleanup failed");
        CHECK(f.symlink.releases == 1);
    }
    {
        Fixture f;
        f.symlink.link_target = "/dir/file";
        f.root.fail_release_at = f.root.releases + 1;
        auto result = f.vfs->resolve({}, "/link");
        CHECK(!result && result.status().message() == "cleanup failed");
        CHECK(f.root.retains == f.root.releases);
    }
    {
        Fixture f;
        CHECK(f.vfs->mount("/dir", f.mounted));
        f.dir.fail_release_at = f.dir.releases + 1;
        auto result = f.vfs->resolve({}, "/dir");
        CHECK(!result && result.status().message() == "cleanup failed");
        CHECK(f.mounted.retains == f.mounted.releases);
    }
    {
        Fixture f;
        f.file.fail_query_at = 1;
        f.file.fail_release_at = 1;
        f.dir.fail_release_at = 1;
        auto result = f.vfs->resolve({}, "/dir/file");
        CHECK(!result && result.status().message() == "primary metadata failure");
        CHECK(f.file.releases == 1 && f.dir.releases == 1);
    }
}

void status_and_mutation_cleanup()
{
    {
        Fixture f;
        f.file.fail_release_at = 1;
        auto status = f.vfs->chmod("/file", 0644);
        CHECK(!status && status.message() == "cleanup failed" && f.file.mutations == 1);
    }
    {
        Fixture f;
        f.file.fail_release_at = 1;
        f.file.fail_mutation = true;
        auto status = f.vfs->chmod("/file", 0644);
        CHECK(!status && status.message() == "primary mutation failure");
    }
    {
        Fixture f;
        f.file.fail_release_at = 1;
        auto result = f.vfs->stat("/file");
        CHECK(!result && result.status().message() == "cleanup failed");
    }
    {
        Fixture f;
        f.dir.fail_release_at = 1;
        auto status = f.vfs->create({}, "/dir/new", fs::NodeType::regular, 0644, 0, 0);
        CHECK(!status && status.message() == "cleanup failed");
        CHECK(f.dir.pins == 0 && f.created.releases == 1 && f.dir.published);
    }
    {
        Fixture f;
        f.file.fail_release_at = 1;
        auto status = f.vfs->unlink("/file");
        CHECK(!status && status.message() == "cleanup failed");
        CHECK(f.root.mutations == 1);
    }
    {
        Fixture f;
        f.root.fail_release_at = f.root.releases + 2; // Destination parent releases before source.
        auto status = f.vfs->link("/file", "/alias");
        CHECK(!status && status.message() == "cleanup failed");
        CHECK(f.root.mutations == 1);
    }
    {
        Fixture f;
        f.file.fail_release_at = 1;
        auto status = f.vfs->rename("/file", "/other");
        CHECK(!status && status.message() == "cleanup failed");
        CHECK(f.root.mutations == 1);
    }
    {
        Fixture f;
        CHECK(f.vfs->mount("/dir", f.mounted));
        f.dir.fail_release_at = f.dir.releases + 1;
        auto status = f.vfs->unlink("/file");
        CHECK(!status && status.message() == "cleanup failed");
        CHECK(f.root.mutations == 0);
    }
    {
        Fixture f;
        f.file.fail_release_at = 1;
        CHECK(f.vfs->lookup("/file") == nullptr);
    }
}

int main()
{
    resolver_handoffs();
    status_and_mutation_cleanup();
    std::puts("PASS VFS release errors: resolver transitions, primary priority, mutation cleanup");
}
