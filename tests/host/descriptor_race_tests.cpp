// Files and IPC locks of smp-scaling WP5 (spec 3.6, 4.13) on the real spin
// locks (OK_HOSTED_LOCK_MODEL) with the lock-order checks on (OK_LOCK_DEBUG):
// a close racing a pinned read, two threads reserving and publishing
// descriptor slots (the openat reservation), a pipe reader arming against a
// writer (no lost wake), and the AF_UNIX rights collection racing a send.
// Host threads stand in for CPUs; no AP starts, no IPI is sent and no guest
// runs (scope=model). The kernel still runs these paths under the big kernel
// lock; the model checks that the fine locks alone keep them correct.
#include "ok/fs/vfs.hpp"
#include "ok/sched/scheduler.hpp"
#include "race_coverage.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#if !defined(OK_HOSTED_LOCK_MODEL)
#error "the descriptor races need the real spin locks"
#endif

using namespace ok;
using namespace ok::sched;
namespace
{
std::atomic<usize> checks{};
#define CHECK(condition)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        checks.fetch_add(1, std::memory_order_relaxed);                                                               \
        if (!static_cast<bool>(condition))                                                                            \
        {                                                                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                      \
            std::abort();                                                                                             \
        }                                                                                                             \
    } while (false)

constexpr usize cpus = 4;

void expect_clean_checks()
{
    for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu) CHECK(smp::lock_violation_count(cpu) == 0);
    CHECK(smp::check_total(smp::KernelCheck::held) == 0);
    CHECK(smp::check_total(smp::KernelCheck::class_rule) == 0);
    // A pipe endpoint count that differed from a scan of the table.
    CHECK(smp::check_total(smp::KernelCheck::audit_repairs) == 0);
}

// One gate per round: every party arrives, then all leave together.
class Barrier
{
  public:
    explicit Barrier(usize parties) : parties_(parties) {}
    void arrive_and_wait()
    {
        const auto generation = generation_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == parties_)
        {
            arrived_.store(0, std::memory_order_relaxed);
            generation_.store(generation + 1, std::memory_order_release);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) std::this_thread::yield();
    }

  private:
    usize parties_;
    std::atomic<usize> arrived_{0};
    std::atomic<usize> generation_{0};
};

// Each side of a race waits a different, round-dependent time after the
// barrier, so both orders of the racing sections occur.
void delay(usize round, usize side)
{
    const usize units = side ? (round * 7 + 3) % 17 : round % 13;
    for (usize spin = 0; spin < units * 150; ++spin) std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Some rounds force the order of the two racing sections (round % 4 == 0:
// side 0 first; == 1: side 1 first) and the others race freely, so both
// orders occur however a loaded host schedules the threads.
class Order
{
  public:
    // Before a side's racing section: waits while the other side goes first.
    void before(usize round, usize side)
    {
        const auto first = round % 4;
        if (first < 2 && first != side)
            while (done_.load(std::memory_order_acquire) != round) std::this_thread::yield();
    }
    // After the section the other side may have to wait for.
    void after(usize round, usize side)
    {
        if (round % 4 == side) done_.store(round, std::memory_order_release);
    }

  private:
    std::atomic<usize> done_{~usize{0}};
};

struct Fixture
{
    std::unique_ptr<Scheduler> owned{std::make_unique<Scheduler>()};
    Scheduler &scheduler{*owned};
    arch::CpuContext context{arch::configured_arch_operations().make_kernel_context(0x1000, 0x8000)};
    usize baseline{};

    Fixture()
    {
        CHECK(scheduler.configure_cpus(cpus));
        CHECK(scheduler.set_online_cpus((1u << cpus) - 1));
        scheduler.set_native_userland(true);
        baseline = scheduler.open_description_count();
    }
    ProcessId process()
    {
        auto pid = scheduler.create_user_process("files", context);
        CHECK(pid);
        scheduler.find_thread(pid.value(), pid.value())->native_active = true;
        CHECK(scheduler.set_runnable(pid.value()));
        return pid.value();
    }
    ProcessResources &resources(ProcessId pid) { return scheduler.find(pid)->resources(); }
    ThreadControlBlock &thread(ProcessId pid) { return *scheduler.find_thread(pid, pid); }
    void run(ProcessId pid, smp::CpuId cpu = 0)
    {
        const auto picked = scheduler.pick(cpu);
        CHECK(picked && picked->ref.pid == pid);
    }
    u16 description(ProcessId pid, i32 fd) { return resources(pid).files.entry(fd)->description; }
};

// close vs pinned read (spec 3.6): one CPU pins the read end of a pipe (as a
// thread of a multi-threaded process does) and reads it, another closes that
// descriptor. The pin keeps the description live, with its node, pipe and
// endpoint count, until it goes; whoever drops the last reference retires it.
void close_versus_pinned_read()
{
    Fixture f;
    const auto pid = f.process();
    auto &resources = f.resources(pid);
    constexpr usize rounds = 20000;
    Barrier barrier{2};
    std::atomic<usize> pinned_rounds{0}, read_bytes{0}, late_pins{0};
    Order order;
    std::array<i32, 2> fds{};
    u16 reader{}, pipe{};
    std::thread closer([&] {
        const smp::ScopedHostedCpu cpu{1};
        for (usize round = 0; round < rounds; ++round)
        {
            barrier.arrive_and_wait();
            delay(round, 1);
            order.before(round, 1);
            CHECK(f.scheduler.close_descriptor(resources, fds[0]));
            order.after(round, 1);
            barrier.arrive_and_wait();
        }
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (usize round = 0; round < rounds; ++round)
        {
            auto created = f.scheduler.create_pipe(resources);
            CHECK(created);
            fds = created.value();
            reader = f.description(pid, fds[0]);
            pipe = f.scheduler.open_description(reader)->pipe;
            const std::byte byte{0x5a};
            CHECK(f.scheduler.write_pipe_fragment(pipe, {&byte, 1}, true));
            barrier.arrive_and_wait();
            delay(round, 0);
            order.before(round, 0);
            auto pinned = f.scheduler.pin_descriptor(resources, fds[0], true);
            // The closer may close the slot now, while the pin holds it.
            order.after(round, 0);
            if (pinned)
            {
                pinned_rounds.fetch_add(1, std::memory_order_relaxed);
                CHECK(pinned.referenced() && pinned.id() == reader);
                // Live while pinned, whatever the closer did meanwhile.
                for (int pass = 0; pass < 4; ++pass)
                {
                    CHECK(pinned->used && pinned->references != 0 && pinned->pipe == pipe && pinned->readable);
                    std::array<std::byte, 4> out{};
                    auto read = f.scheduler.read_pipe(pipe, out, true);
                    if (read) read_bytes.fetch_add(read.value(), std::memory_order_relaxed);
                    auto ready = f.scheduler.pipe_readiness(pipe);
                    CHECK(ready && ready.value().readers && ready.value().writers);
                }
                CHECK(pinned.reset());
            }
            else
                late_pins.fetch_add(1, std::memory_order_relaxed);
            barrier.arrive_and_wait();
            // Both gone: the description is retired; the pipe goes with its writer.
            CHECK(!f.scheduler.open_description(reader) || f.scheduler.open_description(reader)->pipe != pipe ||
                  !f.scheduler.open_description(reader)->references);
            auto ready = f.scheduler.pipe_readiness(pipe);
            CHECK(ready && !ready.value().readers && ready.value().writers);
            CHECK(f.scheduler.close_descriptor(resources, fds[1]));
            CHECK(!f.scheduler.pipe_readiness(pipe));
            CHECK(f.scheduler.open_description_count() == f.baseline);
        }
    }
    closer.join();
    CHECK(pinned_rounds.load() > 0 && late_pins.load() > 0);
    expect_clean_checks();
    std::printf("close_vs_pinned_read rounds=%zu pinned=%zu late=%zu read=%zu\n", rounds, pinned_rounds.load(),
                late_pins.load(), read_bytes.load());
}

// Two threads opening at once (spec 4.13): each reserves the lowest free
// slot under the files lock, does its (here empty) path work, and publishes.
// They never get the same slot, and neither publication overwrites the other.
void two_thread_openat()
{
    Fixture f;
    const auto pid = f.process();
    auto &resources = f.resources(pid);
    constexpr usize rounds = 20000;
    Barrier barrier{2};
    std::array<i32, 2> got{};
    std::atomic<usize> collisions{0}, swapped{0};
    Order order;
    const auto open_one = [&](usize who, usize round) {
        delay(round, who);
        order.before(round, who);
        auto slot = f.scheduler.reserve_descriptor_slot(resources);
        CHECK(slot);
        order.after(round, who);
        delay(round + 5, 1 - who);
        auto description = f.scheduler.allocate_description({});
        CHECK(description);
        auto reserved = slot.value();
        CHECK(f.scheduler.install_reserved_descriptor(resources, reserved, description.value()));
        got[who] = slot.value().fd;
    };
    std::thread second([&] {
        const smp::ScopedHostedCpu cpu{2};
        for (usize round = 0; round < rounds; ++round)
        {
            barrier.arrive_and_wait();
            open_one(1, round);
            barrier.arrive_and_wait();
        }
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (usize round = 0; round < rounds; ++round)
        {
            barrier.arrive_and_wait();
            open_one(0, round);
            barrier.arrive_and_wait();
            if (got[0] == got[1]) collisions.fetch_add(1);
            CHECK(got[0] != got[1]);
            if (got[0] > got[1]) swapped.fetch_add(1);
            for (const auto fd : got)
            {
                const auto *slot = resources.files.entry(fd);
                CHECK(slot && slot->used && !slot->reservation);
                CHECK(f.scheduler.close_descriptor(resources, fd));
            }
            CHECK(f.scheduler.open_description_count() == f.baseline);
        }
    }
    second.join();
    CHECK(collisions.load() == 0 && swapped.load() > 0 && swapped.load() < rounds);
    expect_clean_checks();
    std::printf("two_thread_openat rounds=%zu second_first=%zu\n", rounds, swapped.load());
}

// Pipe no-lost-wake (spec 4.3 A1): a reader that found the pipe empty arms
// under the pipe lock (arm_pipe_wait rechecks there); a writer adds a byte
// under the same lock and wakes the channel. Either the arm's recheck saw the
// byte (the wait completes at once) or the writer's wake found the entry.
void pipe_no_lost_wake()
{
    Fixture f;
    const auto pid = f.process();
    auto &resources = f.resources(pid);
    auto created = f.scheduler.create_pipe(resources);
    CHECK(created);
    const auto pipe = f.scheduler.open_description(f.description(pid, created.value()[0]))->pipe;
    constexpr usize rounds = 20000;
    Barrier barrier{2};
    std::atomic<usize> lost{0}, read_first{0};
    Order order;
    std::thread writer([&] {
        const smp::ScopedHostedCpu cpu{1};
        for (usize round = 0; round < rounds; ++round)
        {
            barrier.arrive_and_wait();
            delay(round, 1);
            order.before(round, 1);
            const std::byte byte{0x21};
            CHECK(f.scheduler.write_pipe_fragment(pipe, {&byte, 1}, true));
            order.after(round, 1);
            barrier.arrive_and_wait();
        }
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (usize round = 0; round < rounds; ++round)
        {
            f.run(pid);
            barrier.arrive_and_wait();
            delay(round, 0);
            order.before(round, 0);
            std::array<std::byte, 1> out{};
            auto read = f.scheduler.read_pipe(pipe, out, true);
            std::optional<WaitTicket> ticket;
            if (read)
                read_first.fetch_add(1, std::memory_order_relaxed);
            else
            {
                CHECK(read.status().code() == StatusCode::would_block);
                auto armed = f.scheduler.arm_pipe_wait(ThreadWaitReason::pipe_read, pipe);
                CHECK(armed);
                ticket = armed.value();
            }
            order.after(round, 0);
            f.scheduler.put_prev(0);
            barrier.arrive_and_wait();
            if (ticket)
            {
                const auto &thread = f.thread(pid);
                if (thread.wait_registration.pending(*ticket)) lost.fetch_add(1);
                CHECK(!thread.wait_registration.pending(*ticket) &&
                      thread.wait_registration.completion() == WaitCompletion::ready);
                // The completed reader takes its byte.
                f.run(pid);
                f.scheduler.settle_wait_links(0);
                CHECK(f.scheduler.read_pipe(pipe, out, true));
                f.scheduler.put_prev(0);
            }
            auto ready = f.scheduler.pipe_readiness(pipe);
            CHECK(ready && ready.value().buffered == 0);
        }
    }
    writer.join();
    CHECK(lost.load() == 0 && read_first.load() > 0 && read_first.load() < rounds);
    CHECK(f.scheduler.wait_channels().linked(static_cast<u8>(ThreadWaitReason::pipe_read), pipe) == 0);
    for (const auto fd : created.value()) CHECK(f.scheduler.close_descriptor(resources, fd));
    expect_clean_checks();
    std::printf("pipe_no_lost_wake rounds=%zu read_first=%zu armed=%zu lost=0\n", rounds, read_first.load(),
                rounds - read_first.load());
}

// The AF_UNIX rights collection against a send (spec 4.13): one CPU passes
// the read end of a pipe over a socket pair, another closes the receiving
// socket and the passed descriptor, a third runs the collection. A right
// stays in flight on a seed pair the whole time (reachable, never collected),
// so every release runs the collection too: the collection's graph build
// races the send's single ofd_table -> pipe hold. The send either fails (peer
// gone) or queues the right; a right left on a stream nobody can read is
// collected. Every round ends with the table and the pipes as they were.
void unix_gc_versus_send()
{
    Fixture f;
    const auto pid = f.process();
    auto &resources = f.resources(pid);
    constexpr usize rounds = 10000;
    // The seed: a pipe's read end in flight on a live pair.
    auto seed = f.scheduler.create_unix_socketpair(resources);
    auto seed_pipe = f.scheduler.create_pipe(resources);
    CHECK(seed && seed_pipe);
    {
        const std::byte payload{0x5a};
        const std::array<i32, 1> rights{seed_pipe.value()[0]};
        CHECK(f.scheduler.send_unix_rights(resources, seed.value()[0], {&payload, 1}, rights));
    }
    const auto baseline = f.scheduler.open_description_count();
    Barrier barrier{3};
    std::array<i32, 2> sockets{}, pipe_fds{};
    std::atomic<usize> sent{0}, refused{0}, collections{0};
    Order order;
    std::thread collector([&] {
        const smp::ScopedHostedCpu cpu{2};
        for (usize round = 0; round < rounds; ++round)
        {
            barrier.arrive_and_wait();
            delay(round, round & 1);
            CHECK(f.scheduler.collect_unix_rights());
            collections.fetch_add(1, std::memory_order_relaxed);
            barrier.arrive_and_wait();
        }
    });
    std::thread closer([&] {
        const smp::ScopedHostedCpu cpu{1};
        for (usize round = 0; round < rounds; ++round)
        {
            barrier.arrive_and_wait();
            delay(round, 1);
            order.before(round, 1);
            CHECK(f.scheduler.close_descriptor(resources, sockets[1]));
            CHECK(f.scheduler.close_descriptor(resources, pipe_fds[0]));
            order.after(round, 1);
            barrier.arrive_and_wait();
        }
    });
    {
        const smp::ScopedHostedCpu cpu{0};
        for (usize round = 0; round < rounds; ++round)
        {
            auto pair = f.scheduler.create_unix_socketpair(resources);
            auto pipe = f.scheduler.create_pipe(resources);
            CHECK(pair && pipe);
            sockets = pair.value();
            pipe_fds = pipe.value();
            const auto passed_pipe = f.scheduler.open_description(f.description(pid, pipe_fds[0]))->pipe;
            barrier.arrive_and_wait();
            delay(round, 0);
            order.before(round, 0);
            const std::byte payload{0x7e};
            const std::array<i32, 1> rights{pipe_fds[0]};
            auto result = f.scheduler.send_unix_rights(resources, sockets[0], {&payload, 1}, rights);
            order.after(round, 0);
            (result ? sent : refused).fetch_add(1, std::memory_order_relaxed);
            barrier.arrive_and_wait();
            CHECK(f.scheduler.close_descriptor(resources, sockets[0]));
            CHECK(f.scheduler.close_descriptor(resources, pipe_fds[1]));
            // Nothing of the round is left: no description, no right, no
            // buffer; the seed's right is still in flight.
            CHECK(f.scheduler.open_description_count() == baseline);
            CHECK(!f.scheduler.pipe_readiness(passed_pipe));
        }
    }
    closer.join();
    collector.join();
    CHECK(sent.load() > 0 && refused.load() > 0 && collections.load() == rounds);
    // The seed pair and its pipe go; the right in flight is collected with them.
    for (const auto fd : {seed.value()[0], seed.value()[1], seed_pipe.value()[0], seed_pipe.value()[1]})
        CHECK(f.scheduler.close_descriptor(resources, fd));
    CHECK(f.scheduler.open_description_count() == f.baseline);
    expect_clean_checks();
    std::printf("unix_gc_vs_send rounds=%zu sent=%zu refused=%zu collections=%zu\n", rounds, sent.load(),
                refused.load(), collections.load());
}
// tty commit retry (spec 4.13): two CPUs read one PTY master as prepare
// (tty lock), copy-out (no lock) and commit (tty lock, revision check) while
// a third writes the slave. A reader whose preview another reader overtook
// fails its commit and prepares again; every byte is read exactly once, each
// read a contiguous run of what was written.
void tty_commit_retry()
{
    Fixture f;
    const auto pid = f.process();
    auto &resources = f.resources(pid);
    const smp::ScopedHostedCpu boot{0};
    auto prepared = f.scheduler.prepare_pty_pair(pid, 0x0800u);
    CHECK(prepared);
    CHECK(prepared.value().validate());
    prepared.value().publish_checked();
    const auto fds = prepared.value().descriptors();
    const auto master = f.description(pid, fds[0]);
    const auto slave = f.description(pid, fds[1]);
    const auto endpoint = f.scheduler.open_description(master)->pty_endpoint;
    constexpr usize total = 200000;
    // Printable bytes only: output processing changes nothing else.
    const auto value = [](usize position) { return static_cast<std::byte>(32 + position % 95); };
    std::atomic<usize> read{0}, copies{0}, reads{0}, broken{0};
    std::vector<std::thread> readers;
    for (smp::CpuId cpu = 1; cpu <= 2; ++cpu)
        readers.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu hosted{cpu};
            std::array<std::byte, 7> out{};
            while (read.load(std::memory_order_acquire) < total)
            {
                usize staged = 0;
                auto got = f.scheduler.read_pty_prepared(master, endpoint, out.size(), false, {},
                    [&](std::span<const std::byte> bytes) -> Status {
                        copies.fetch_add(1, std::memory_order_relaxed);
                        for (usize index = 0; index < bytes.size(); ++index) out[index] = bytes[index];
                        staged = bytes.size();
                        return {};
                    });
                if (!got)
                {
                    if (got.status().code() != StatusCode::would_block)
                        std::fprintf(stderr, "pty read: %u %.*s\n", static_cast<unsigned>(got.status().code()),
                                     static_cast<int>(got.status().message().size()), got.status().message().data());
                    CHECK(got.status().code() == StatusCode::would_block);
                    std::this_thread::yield();
                    continue;
                }
                CHECK(got.value() == staged && staged != 0);
                // A contiguous run of the written sequence.
                for (usize index = 1; index < staged; ++index)
                    if ((std::to_integer<usize>(out[index]) - 32) != (std::to_integer<usize>(out[index - 1]) - 32 + 1) % 95)
                        broken.fetch_add(1);
                reads.fetch_add(1, std::memory_order_relaxed);
                read.fetch_add(staged, std::memory_order_acq_rel);
            }
        });
    std::array<std::byte, 64> chunk{};
    for (usize written = 0; written < total;)
    {
        const auto count = total - written < chunk.size() ? total - written : chunk.size();
        for (usize index = 0; index < count; ++index) chunk[index] = value(written + index);
        auto result = f.scheduler.write_pty_description(slave, {chunk.data(), count});
        if (result) written += result.value();
        else
        {
            CHECK(result.status().code() == StatusCode::would_block);
            std::this_thread::yield();
        }
    }
    for (auto &thread : readers) thread.join();
    CHECK(read.load() == total && broken.load() == 0);
    // A preview another read overtook fails its commit (busy), whatever the
    // host's scheduling did above: stage four bytes, take two, commit.
    const std::array<std::byte, 8> eight{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'},
                                         std::byte{'e'}, std::byte{'f'}, std::byte{'g'}, std::byte{'h'}};
    CHECK(f.scheduler.write_pty_description(slave, eight));
    std::array<std::byte, 8> staged{};
    std::optional<Scheduler::PreparedPtyRead> preview;
    auto planned = f.scheduler.stage_pty_read(master, endpoint, 4, false, {}, staged, &preview);
    CHECK(planned && planned.value() == 4 && preview && staged[0] == std::byte{'a'});
    std::array<std::byte, 2> two{};
    CHECK(f.scheduler.read_pty_description(master, two) && two[1] == std::byte{'b'});
    auto stale = f.scheduler.commit_pty_read(*preview);
    CHECK(!stale && stale.status().code() == StatusCode::busy);
    preview->cancel();
    // The retrying read then gets the bytes after the overtaking read's.
    auto retried = f.scheduler.read_pty_prepared(master, endpoint, 6, false, {}, [&](std::span<const std::byte> bytes) {
        CHECK(bytes.size() == 6 && bytes[0] == std::byte{'c'} && bytes[5] == std::byte{'h'});
        return Status{};
    });
    CHECK(retried && retried.value() == 6);
    for (const auto fd : fds) CHECK(f.scheduler.close_descriptor(resources, fd));
    expect_clean_checks();
    std::printf("tty_commit_retry bytes=%zu reads=%zu retried_copies=%zu\n", total, reads.load(),
                copies.load() - reads.load());
}
// RAM rename vs lookup (spec 4.13): with the namespace gate shared, one CPU
// moves a directory between two parents while two others resolve both paths
// and name what they found. The ram_topology lock makes the rename atomic:
// a lookup finds the directory under exactly the name it asked for, and its
// path from the root is always one of the two whole names.
void ram_rename_versus_lookup()
{
    auto vfs = std::make_unique<fs::VirtualFileSystem>();
    {
        const smp::ScopedHostedCpu boot{0};
        CHECK(vfs->create("/a", fs::NodeType::directory));
        CHECK(vfs->create("/b", fs::NodeType::directory));
        CHECK(vfs->create("/a/d", fs::NodeType::directory));
    }
    // Renames continue past the usual budget until lookups both found and
    // missed the directory (race_coverage.hpp).
    test::CoverageRounds renames{20000, 200000};
    std::atomic<bool> stop{false};
    std::atomic<usize> found{0}, missed{0}, moved{0};
    std::vector<std::thread> lookers;
    for (smp::CpuId cpu = 2; cpu <= 3; ++cpu)
        lookers.emplace_back([&, cpu] {
            const smp::ScopedHostedCpu hosted{cpu};
            const std::array<std::string_view, 2> paths{"/a/d", "/b/d"};
            for (usize pass = 0; !stop.load(std::memory_order_acquire); ++pass)
            {
                const auto path = paths[(pass + cpu) % 2];
                auto resolved = vfs->resolve({}, path);
                if (!resolved)
                {
                    CHECK(resolved.status().code() == StatusCode::not_found);
                    missed.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                auto reference = std::move(resolved.value());
                auto named = vfs->directory_path(*reference.node(), reference.handle().mount_id);
                if (!named)
                {
                    // Moved between the walk and its re-resolution: not torn.
                    CHECK(named.status().code() == StatusCode::not_found);
                    moved.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    // Moved on since the lookup is fine; a torn name is not.
                    CHECK(named.value().view() == "/a/d" || named.value().view() == "/b/d");
                    found.fetch_add(1, std::memory_order_relaxed);
                }
                CHECK(reference.release());
            }
        });
    {
        const smp::ScopedHostedCpu cpu{1};
        while (renames.next(found.load() > 0 && missed.load() > 0))
        {
            CHECK(vfs->rename("/a/d", "/b/d"));
            // A nudged extension round lets the lookers run mid-move.
            if (renames.nudged()) test::nudge();
            CHECK(vfs->rename("/b/d", "/a/d"));
        }
        stop.store(true, std::memory_order_release);
    }
    for (auto &thread : lookers) thread.join();
    std::printf("ram_rename_vs_lookup renames=%zu min_renames=%zu found=%zu missed=%zu moved=%zu\n",
                2 * renames.rounds(), 2 * renames.minimum(), found.load(), missed.load(), moved.load());
    std::fflush(stdout);
    {
        const smp::ScopedHostedCpu boot{0};
        CHECK(vfs->resolve({}, "/a/d") && !vfs->resolve({}, "/b/d"));
    }
    CHECK(found.load() > 0 && missed.load() > 0);
    expect_clean_checks();
}
// Atomic O_APPEND (spec 4.13, Node::append): two CPUs append 8-byte records
// to one RAM file through the node, as two descriptions' O_APPEND writes do.
// Each append reads the end and writes under one node lock hold: no record
// is torn or overwritten, and each returned offset is its record's.
void append_versus_append()
{
    auto vfs = std::make_unique<fs::VirtualFileSystem>();
    fs::Node *node{};
    {
        const smp::ScopedHostedCpu boot{0};
        CHECK(vfs->create("/log", fs::NodeType::regular));
        node = vfs->lookup("/log");
        CHECK(node);
    }
    constexpr usize records = fs::max_file_data / 8 / 2;
    std::array<std::array<usize, records>, 2> offsets{};
    std::vector<std::thread> writers;
    for (usize side = 0; side < 2; ++side)
        writers.emplace_back([&, side] {
            const smp::ScopedHostedCpu hosted{static_cast<smp::CpuId>(side + 1)};
            std::array<std::byte, 8> record{};
            for (usize index = 0; index < records; ++index)
            {
                record.fill(static_cast<std::byte>(side ? 0xb0 + index % 16 : 0x10 + index % 16));
                auto appended = node->append(record);
                CHECK(appended && appended.value() % 8 == 0);
                offsets[side][index] = appended.value();
            }
        });
    for (auto &thread : writers) thread.join();
    const smp::ScopedHostedCpu boot{0};
    auto metadata = node->query_metadata();
    CHECK(metadata && metadata.value().size == 2 * records * 8);
    for (usize side = 0; side < 2; ++side)
        for (usize index = 0; index < records; ++index)
        {
            auto bytes = node->read(offsets[side][index], 8);
            CHECK(bytes && bytes.value().size == 8);
            const auto expected = static_cast<std::byte>(side ? 0xb0 + index % 16 : 0x10 + index % 16);
            for (usize at = 0; at < 8; ++at) CHECK(bytes.value().data[at] == expected);
        }
    expect_clean_checks();
    std::printf("append_vs_append records=%zu size=%zu\n", 2 * records, metadata.value().size);
}
} // namespace

int main()
{
    close_versus_pinned_read();
    two_thread_openat();
    pipe_no_lost_wake();
    unix_gc_versus_send();
    tty_commit_retry();
    ram_rename_versus_lookup();
    append_versus_append();
    std::printf("OK_DESCRIPTOR_RACES scope=model checks=%zu model_cpus=%zu guest=0 hardware_smp=0\n", checks.load(),
                cpus);
}
