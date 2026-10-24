// Actual POSIX/VFS/production CommonStorage reproduction of shell split writes.
#define OK_CHECKED_OWNERSHIP_NO_MAIN
#include "ext4_checked_ownership_tests.cpp"

struct AccountWriteMount
{
    Disk disk;
    Mount::Registry registry;
    std::unique_ptr<Mount::CommonStorage> storage = std::make_unique<Mount::CommonStorage>();
    Mount mount{*storage};
    explicit AccountWriteMount(const Bytes &bytes) : disk(bytes)
    {
        auto key = registry.register_device(disk, 17); CHECK(key);
        CHECK(mount.mount(registry, key.value(), {0, bytes.size() / 512}));
    }
};

void account_write_status(std::string_view stage, AccountWriteMount &mounted, Status status)
{
    const auto report = mounted.mount.report();
    std::fprintf(stderr, "ACCOUNT_WRITE stage=%.*s result=%u message=%.*s phase=%u mount_status=%u mount_message=%.*s\n",
        int(stage.size()), stage.data(), unsigned(status.code()), int(status.message().size()), status.message().data(),
        unsigned(report.phase), unsigned(report.status.code()), int(report.status.message().size()), report.status.message().data());
}

void account_split_write(const Bytes &clean, u32 uid)
{
    clock_model::set(sample()); auto mounted = std::make_unique<AccountWriteMount>(clean);
    auto services = std::make_unique<Services>(*mounted->mount.root()); auto &p = services->service;
    CHECK(p.set_credentials(user::root_credentials()));
    CHECK(p.mkdir("/journal/diag-home", 0755));
    CHECK(p.mkdir("/journal/diag-home/alice", 0700));
    auto home = p.open("/journal/diag-home/alice", posix::o_RDONLY | posix::o_DIRECTORY); CHECK(home);
    CHECK(p.change_owner_fd(home.value(), uid, uid).ok());
    CHECK(p.change_mode_fd(home.value(), 0700).ok());
    CHECK(p.set_identity(uid, uid));
    CHECK(p.fchdir(home.value()) && p.close(home.value()));
    static_cast<void>(p.umask(0077));
    auto output = p.open("kept", posix::o_WRONLY | posix::o_CREAT | posix::o_TRUNC, 0600);
    account_write_status("create", *mounted, output ? Status{} : output.status()); CHECK(output);
    auto before = p.fstat(output.value()); CHECK(before);
    CHECK(before.value().uid == uid && before.value().gid == uid && before.value().size == 0);
    auto backup = p.duplicate(1); CHECK(backup);
    auto redirect = p.duplicate_to(output.value(), 1);
    account_write_status("dup2-output", *mounted, redirect ? Status{} : redirect.status()); CHECK(redirect);
    CHECK(p.close(output.value()));
    constexpr std::string_view body = "ordinary-kept";
    auto first = p.write(1, std::as_bytes(std::span{body.data(), body.size()}));
    account_write_status("write-body", *mounted, first ? Status{} : first.status()); CHECK(first && first.value() == 13);
    clock_model::set(sample(1700000078, 987654321));
    constexpr std::array<std::byte, 1> newline{std::byte{'\n'}};
    auto second = p.write(1, newline);
    account_write_status("write-newline", *mounted, second ? Status{} : second.status()); CHECK(second && second.value() == 1);
    CHECK(p.duplicate_to(backup.value(), 1) && p.close(backup.value()));
    account_write_status("restore-close", *mounted, {});
    auto status = p.stat("kept"); CHECK(status && status.value().size == 14);
    CHECK(status.value().uid == uid && status.value().gid == uid && status.value().mode == 0100600);
    auto input = p.open("kept", posix::o_RDONLY); CHECK(input);
    std::array<std::byte, 14> actual{};
    auto read = p.read(input.value(), actual); CHECK(read && read.value() == actual.size());
    for (usize index = 0; index < body.size(); ++index) CHECK(actual[index] == std::byte(body[index]));
    CHECK(actual.back() == std::byte{'\n'} && p.close(input.value()));
    CHECK(mounted->mount.report().phase == Ext4MountPhase::readable);
    auto forked = p.fork(); account_write_status("fork", *mounted, forked ? Status{} : forked.status()); CHECK(forked);
    CHECK(services->scheduler.exit_process(forked.value(), 0));
    CHECK(p.wait(services->pid, forked.value()));
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); std::ifstream input(argv[1], std::ios::binary); CHECK(input);
    const Bytes clean((std::istreambuf_iterator<char>(input)), {}); CHECK(!clean.empty());
    account_split_write(clean, 0); account_split_write(clean, 1000);
    std::printf("ACCOUNT_SPLIT_WRITES checks=%zu root=1 ordinary=1 production_common_storage=1 hosted=1 guest=0 hardware=0\n", checks);
}
