// Hosted fallible VFS boundary tests; no guest CPU or device transport.
#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
using namespace ok;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

class CheckedNode final : public fs::Node
{
  public:
    fs::NodeType type{fs::NodeType::regular};
    CheckedNode *child{};
    fs::VirtualFileSystem *vfs{};
    mutable usize queries{}, reads{}, listings{}, callback_checks{};
    usize pins{}, retains{}, releases{}, writes{}, mutations{}, lookups{};
    usize fail_query{}, fail_lookup{}, fail_release{};
    std::string_view fail_lookup_name{};
    std::string_view mutation_success{}, cleanup_success{};
    bool fail_write{}, fail_mutation{}, fail_read{}, fail_list{}, reenter{};
    usize last_write_offset{};
    std::string_view name() const override
    {
        return "checked";
    }
    fs::Metadata metadata() const override
    {
        return {.type = type, .size = 6, .mode = fs::mode_for(type, 0777)};
    }
    Result<fs::Metadata> query_metadata() const override
    {
        if (++queries == fail_query)
            return Status::io_error("metadata read failed");
        return metadata();
    }
    fs::Node *lookup(std::string_view name) override
    {
        return name == "file" ? child : name == ".." ? this : nullptr;
    }
    Result<fs::Node *> lookup_child(std::string_view name) override
    {
        ++lookups;
        if (lookups == fail_lookup || (!fail_lookup_name.empty() && name == fail_lookup_name))
            return Status::io_error("directory read failed");
        auto *node = lookup(name);
        return node ? Result<fs::Node *>{node} : Result<fs::Node *>{Status::not_found()};
    }
    Status retain() override
    {
        ++retains;
        ++pins;
        return Status::success();
    }
    void check_callback() const
    {
        if (!reenter) return;
        CHECK(vfs);
        auto stat = vfs->stat("/file");
        auto listing = vfs->list("/");
        CHECK(!stat && stat.status().code() == StatusCode::busy);
        CHECK(!listing && listing.status().code() == StatusCode::busy);
        CHECK(vfs->write_file("/file", {}).code() == StatusCode::busy);
        ++callback_checks;
    }
    Status release() override
    {
        check_callback();
        CHECK(pins);
        --pins;
        return ++releases == fail_release ? Status::io_error("cleanup failed") : Status{StatusCode::ok, cleanup_success};
    }
    Result<fs::FileBuffer> read(usize, usize count) const override
    {
        check_callback();
        ++reads;
        if (fail_read) return Status::io_error("file read failed");
        fs::FileBuffer out{};
        out.size = count < 6 ? count : 6;
        for (usize i = 0; i < out.size; ++i) out.data[i] = static_cast<std::byte>("abcdef"[i]);
        return out;
    }
    Status write(usize offset, std::span<const std::byte>) override
    {
        ++writes;
        last_write_offset = offset;
        return fail_write ? Status::no_space("write failed") : Status::success();
    }
    Status change()
    {
        ++mutations;
        return fail_mutation ? Status::denied("mutation failed") : Status{StatusCode::ok, mutation_success};
    }
    Status create(std::string_view, fs::NodeType) override
    {
        return change();
    }
    Status remove(std::string_view, bool) override
    {
        return change();
    }
    Status rename(std::string_view, fs::Node &, std::string_view) override
    {
        return change();
    }
    Status link(std::string_view, fs::Node &) override
    {
        return change();
    }
    Status chmod(u32) override
    {
        return change();
    }
    Status chown(u32, u32) override
    {
        return change();
    }
    Result<fs::DirectoryListing> list(fs::DirectoryReadIntent = fs::DirectoryReadIntent::access) const override
    {
        check_callback();
        ++listings;
        if (fail_list) return Status::io_error("listing read failed");
        Result<fs::DirectoryListing> result{std::in_place};
        if (child)
        {
            result.value().count = 1;
            CHECK(result.value().entries[0].name.assign("file"));
            result.value().entries[0].metadata = child->metadata();
        }
        return result;
    }
};
struct Fixture
{
    CheckedNode root, file;
    sched::Scheduler scheduler;
    fs::VirtualFileSystem vfs;
    driver::ConsoleDriver console;
    posix::PosixService service;
    u32 pid{};
    Fixture()
    {
        root.type = fs::NodeType::directory;
        root.child = &file;
        CHECK(vfs.mount("/", root));
        CHECK(console.start());
        CHECK(service.initialize(vfs, console, scheduler));
        auto process = scheduler.create_user_process(
            "checked-vfs", arch::configured_arch_operations().make_user_context({0x400000, 0x800000, 0}));
        CHECK(process);
        pid = process.value();
        CHECK(service.activate(pid));
        root.vfs = file.vfs = &vfs;
        root.queries = root.lookups = 0;
    }
    sched::OpenFileDescription *description(posix::Fd fd)
    {
        auto *slot = scheduler.find(pid)->resources().files.entry(fd);
        return slot && slot->used ? scheduler.open_description(slot->description) : nullptr;
    }
    ~Fixture()
    {
        root.reenter = file.reenter = false;
        for (posix::Fd fd = 3; fd < static_cast<posix::Fd>(posix::max_open_files); ++fd)
            if (description(fd))
                static_cast<void>(service.close(fd));
        auto &resources = scheduler.find(pid)->resources();
        if (resources.cwd_node)
        {
            static_cast<void>(resources.cwd_node->release());
            resources.cwd_node = nullptr;
        }
        CHECK(file.pins == 0 && root.pins == 0);
    }
};
void descriptors()
{
    auto f = std::make_unique<Fixture>();
    auto opened = f->service.open("/file", posix::o_RDWR | posix::o_APPEND);
    CHECK(opened);
    auto fd = opened.value();
    auto *description = f->description(fd);
    CHECK(description);
    description->offset = 2;
    std::array<std::byte, 1> bytes{};
    f->file.fail_query = f->file.queries + 1;
    auto write = f->service.write(fd, bytes);
    CHECK(!write && write.status().code() == StatusCode::io_error);
    CHECK(description->offset == 2 && f->file.writes == 0);
    f->file.fail_query = 0;
    f->file.fail_write = true;
    write = f->service.write(fd, bytes);
    CHECK(!write && write.status().code() == StatusCode::no_space);
    CHECK(description->offset == 2 && f->file.last_write_offset == 6);
    f->file.fail_write = false;
    CHECK(f->service.write(fd, bytes));
    CHECK(description->offset == 7);
    f->file.fail_query = f->file.queries + 1;
    auto seek = f->service.seek(fd, 0, posix::SeekWhence::end);
    CHECK(!seek && seek.status().code() == StatusCode::io_error);
    CHECK(description->offset == 7);
    f->file.fail_query = f->file.queries + 1;
    CHECK(f->service.seek(fd, 2, posix::SeekWhence::set));
    CHECK(f->service.seek(fd, 1, posix::SeekWhence::current));
    CHECK(f->file.queries + 1 == f->file.fail_query && description->offset == 3);
    auto stat = f->service.fstat(fd);
    CHECK(!stat && stat.status().code() == StatusCode::io_error);
    f->file.fail_query = f->file.queries + 1;
    auto ready = f->service.ready_events(fd, posix::ReadyEvents::readable);
    CHECK(!ready && ready.status().code() == StatusCode::io_error);
}
void path_cleanup()
{
    for (int operation = 0; operation < 4; ++operation)
    {
        auto f = std::make_unique<Fixture>();
        if (operation == 3)
            f->file.type = fs::NodeType::symlink;
        f->file.fail_release = 1;
        std::array<std::byte, 8> bytes{};
        Status result;
        if (operation == 0)
            result = f->service.stat("/file").status();
        if (operation == 1)
            result = f->service.chmod("/file", 0600);
        if (operation == 2)
            result = f->service.chown("/file", 0, 0);
        if (operation == 3)
            result = f->service.readlinkat(posix::at_FDCWD, "/file", bytes).status();
        CHECK(!result && result.message() == "cleanup failed" && f->file.pins == 0 && f->file.releases == 1);
    }
    auto f = std::make_unique<Fixture>();
    f->file.fail_query = 2;
    f->file.fail_release = 1;
    auto stat = f->service.stat("/file");
    CHECK(!stat && stat.status().message() == "metadata read failed");
    CHECK(f->file.releases == 1 && f->file.pins == 0);
    f->file.fail_query = 0;
    f->file.fail_mutation = true;
    f->file.fail_release = 2;
    auto status = f->service.chmod("/file", 0600);
    CHECK(!status && status.message() == "mutation failed");
    CHECK(f->file.releases == 2 && f->file.pins == 0);
}
void directory_descriptors()
{
    auto f = std::make_unique<Fixture>();
    f->file.type = fs::NodeType::directory;
    auto opened = f->service.open("/file", posix::o_RDONLY);
    CHECK(opened);
    auto fd = opened.value();
    for (int operation = 0; operation < 3; ++operation)
    {
        f->file.fail_query = f->file.queries + 1;
        std::array<std::byte, 64> bytes{};
        Status result;
        if (operation == 0)
            result = f->service.getdents64(fd, bytes).status();
        if (operation == 1)
            result = f->service.fchdir(fd);
        if (operation == 2)
            result = f->service.openat(fd, "missing", posix::o_RDONLY).status();
        CHECK(!result && result.code() == StatusCode::io_error && f->description(fd)->offset == 0);
    }
    f->file.fail_query = 0;
    f->file.fail_release = f->file.releases + 1;
    auto cwd = f->scheduler.find(f->pid)->resources().cwd_node;
    auto status = f->service.chdir("/file");
    CHECK(!status && status.message() == "cleanup failed");
    CHECK(f->scheduler.find(f->pid)->resources().cwd_node == cwd && f->file.pins == 1);
    f->file.fail_release = 0;
    CHECK(f->service.fchdir(fd));
    CHECK(f->file.pins == 2);
}
void namespace_errors()
{
    for (int operation = 0; operation < 6; ++operation)
    {
        auto f = std::make_unique<Fixture>();
        f->root.fail_lookup_name = "file";
        Status result;
        if (operation == 0)
            result = f->vfs.stat("/file").status();
        if (operation == 1)
            result = f->vfs.read_file("/file").status();
        if (operation == 2)
            result = f->vfs.chmod("/file", 0600);
        if (operation == 3)
            result = f->vfs.chown("/file", 0, 0);
        if (operation == 4)
            result = f->vfs.unlink("/file");
        if (operation == 5)
            result = f->vfs.rename("/file", "/missing");
        CHECK(!result && result.message() == "directory read failed" && f->root.mutations == 0 &&
              f->file.mutations == 0);
    }
    auto f = std::make_unique<Fixture>();
    f->root.fail_lookup_name = "missing";
    auto created = f->vfs.create("/missing", fs::NodeType::regular);
    CHECK(!created && created.message() == "directory read failed" && f->root.mutations == 0);
    auto renamed = f->vfs.rename("/file", "/missing");
    CHECK(!renamed && renamed.message() == "directory read failed" && f->root.mutations == 0);
    auto linked = f->vfs.link("/file", "/missing");
    CHECK(!linked && linked.message() == "directory read failed" && f->root.mutations == 0);
    f->root.fail_lookup_name = {};
    linked = f->vfs.link("/file", "/file");
    CHECK(!linked && linked.code() == StatusCode::already_exists && f->root.mutations == 0);
    linked = f->vfs.link("/file", "/missing");
    CHECK(linked && f->root.mutations == 1);
}

void retained_payload_cleanup()
{
    // The acquired path hold is consumed once, including an erroring release.
    for (int kind = 0; kind < 4; ++kind)
        for (bool primary : {false, true})
            for (bool cleanup : {false, true})
            {
                auto f = std::make_unique<Fixture>();
                auto &node = kind == 0 ? f->root : f->file;
                const auto pins = node.pins, retains = node.retains, releases = node.releases;
                node.reenter = true;
                if (kind == 3)
                {
                    node.mutation_success = "mutation succeeded";
                    node.cleanup_success = "cleanup succeeded";
                }
                if (cleanup) node.fail_release = releases + 1;
                if (primary)
                {
                    if (kind == 0) node.fail_list = true;
                    if (kind == 1) node.fail_read = true;
                    if (kind == 2) node.fail_query = node.queries + 2; // Resolver query precedes stat query.
                    if (kind == 3) node.fail_mutation = true;
                }
                Status observed;
                if (kind == 0)
                {
                    auto result = f->vfs.list("/");
                    observed = result.status();
                    if (result)
                        CHECK(result.value().count == 1 && result.value().entries[0].name.view() == "file" &&
                              result.value().entries[0].metadata.type == fs::NodeType::regular &&
                              result.value().entries[0].metadata.size == 6);
                    CHECK(node.listings == 1 && node.callback_checks == 2);
                }
                if (kind == 1)
                {
                    auto result = f->vfs.read_file("/file");
                    observed = result.status();
                    if (result)
                    {
                        CHECK(result.value().size == 6);
                        for (usize i = 0; i < 6; ++i)
                            CHECK(result.value().data[i] == static_cast<std::byte>("abcdef"[i]));
                    }
                    CHECK(node.reads == 1 && node.callback_checks == 2);
                }
                if (kind == 2)
                {
                    auto result = f->vfs.stat("/file");
                    observed = result.status();
                    if (result) CHECK(result.value().size == 6 && result.value().type == fs::NodeType::regular);
                }
                if (kind == 3) observed = f->vfs.chmod("/file", 0600);
                if (primary)
                {
                    constexpr std::string_view errors[]{"listing read failed", "file read failed",
                                                        "metadata read failed", "mutation failed"};
                    CHECK(!observed && observed.message() == errors[kind]);
                }
                else if (cleanup) CHECK(!observed && observed.message() == "cleanup failed");
                else
                {
                    CHECK(observed);
                    if (kind == 3) CHECK(observed.message() == "cleanup succeeded");
                }
                CHECK(node.retains == retains + 1 && node.releases == releases + 1 && node.pins == pins);
                CHECK(node.callback_checks >= 1);
                node.fail_list = node.fail_read = node.fail_mutation = false;
                node.fail_query = node.fail_release = 0;
                CHECK(f->vfs.stat("/file")); // Both failure paths release operation admission.
            }
    {
        auto f = std::make_unique<Fixture>();
        f->file.fail_query = f->file.queries + 2;
        f->file.fail_release = f->file.releases + 1;
        auto result = f->vfs.read_file("/file");
        CHECK(!result && result.status().message() == "metadata read failed");
        CHECK(f->file.reads == 0 && f->file.retains == 1 && f->file.releases == 1 && !f->file.pins);
        f->file.fail_query = f->file.fail_release = 0;
        auto first = f->vfs.read_file("/file");
        auto second = f->vfs.read_file("/file");
        CHECK(first && second && first.value().size == 6 && second.value().size == 6);
        first.value().data[0] = std::byte{'z'};
        first.value().size = 1;
        CHECK(second.value().data[0] == std::byte{'a'} && second.value().size == 6);
    }
}

void ram_listing_storage()
{
    static_assert(fs::max_child_nodes == 64);
    auto vfs = std::make_unique<fs::VirtualFileSystem>();
    auto root = std::make_unique<fs::RamNode>();
    auto children = std::make_unique<std::array<fs::RamNode, fs::max_child_nodes + 1>>();
    CHECK(root->configure("/", fs::NodeType::directory, nullptr));
    CHECK(vfs->mount("/", *root));
    auto empty_direct = root->list();
    auto empty_vfs = vfs->list("/");
    CHECK(empty_direct && empty_vfs && !empty_direct.value().count && !empty_vfs.value().count);
    empty_direct.value().count = 1;
    CHECK(empty_vfs.value().count == 0);
    const std::array<std::byte, 4> data{std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'}};
    for (usize i = 0; i < children->size(); ++i)
    {
        char name[16];
        CHECK(std::snprintf(name, sizeof(name), "entry-%02zu", i) > 0);
        CHECK((*children)[i].configure_readonly(name, std::span(data).first(i % 5), 0600 | (i % 8)));
        CHECK((*children)[i].chown(1000 + i, 2000 + i));
        auto attached = root->attach_child((*children)[i]);
        if (i < fs::max_child_nodes) CHECK(attached);
        else CHECK(!attached && attached.code() == StatusCode::no_memory);
    }
    auto full_direct = root->list();
    auto full_vfs = vfs->list("/");
    CHECK(full_direct && full_vfs && full_direct.value().count == 64 && full_vfs.value().count == 64);
    for (usize i = 0; i < fs::max_child_nodes; ++i)
    {
        char name[16];
        CHECK(std::snprintf(name, sizeof(name), "entry-%02zu", i) > 0);
        for (const auto *listing : {&full_direct.value(), &full_vfs.value()})
        {
            const auto &entry = listing->entries[i];
            CHECK(entry.name.view() == name && entry.metadata.type == fs::NodeType::regular);
            CHECK(entry.metadata.size == i % 5 && entry.metadata.mode == fs::mode_for(fs::NodeType::regular, 0600 | (i % 8)));
            CHECK(entry.metadata.uid == 1000 + i && entry.metadata.gid == 2000 + i && entry.metadata.link_count == 1);
            CHECK(entry.metadata.block_size == fs::metadata_block_size && entry.metadata.blocks == (i % 5 ? 1u : 0u));
        }
    }
    CHECK(full_direct.value().entries[0].name.assign("changed"));
    full_direct.value().entries[0].metadata.uid = 99;
    CHECK(full_vfs.value().entries[0].name.view() == "entry-00" && full_vfs.value().entries[0].metadata.uid == 1000);
    CHECK(empty_vfs.value().count == 0);
    auto invalid = (*children)[0].list();
    CHECK(!invalid && invalid.status().code() == StatusCode::invalid_argument);
}
int main()
{
    descriptors();
    path_cleanup();
    directory_descriptors();
    namespace_errors();
    retained_payload_cleanup();
    ram_listing_storage();
    std::puts("PASS checked VFS models: metadata/lookup errors, append/seek offsets, explicit cleanup, cwd pins and no "
              "mutation after admission failure; retained payload priority/reentry/success status and independent RAM listings 0/64");
}
