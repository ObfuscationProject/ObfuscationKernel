// Hosted model test for the kernel section protocol (ok/smp/kernel_section.hpp).
// Threads stand in for CPUs through ScopedHostedCpu; no section body, user
// process or interrupt executes. Built with OK_LOCK_DEBUG (depth assertions)
// and without it.
#include "ok/smp/cpu.hpp"
#include "ok/smp/kernel_section.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace ok;

namespace
{

int checks = 0;

void expect(bool condition, const char *what)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s\n", what);
        std::exit(1);
    }
}

// What the commit handler saw: which record, its result and state, the CPU
// it ran on and whether that CPU held the kernel lock.
struct Commit
{
    const smp::SectionRecord *record{};
    i64 result{};
    smp::SectionState state{};
    smp::CpuId cpu{};
    bool locked{};
};
std::atomic<int> commits{0};
Commit last_commit{};

void record_commit(smp::SectionRecord &record) noexcept
{
    last_commit = {&record, record.result, record.load(), smp::current_cpu(), smp::kernel_lock.owned_by_current_cpu()};
    commits.fetch_add(1);
}

uptr total(std::atomic<uptr> smp::SectionCpu::*counter)
{
    uptr sum = 0;
    for (const auto &cpu : smp::section_cpus)
        sum += (cpu.*counter).load();
    return sum;
}

void wait_until(const std::atomic<bool> &flag)
{
    while (!flag.load())
        std::this_thread::yield();
}

// Polls `condition` for a while; false if it never held.
template <typename Condition> bool eventually(Condition condition)
{
    for (int spin = 0; spin < 2000000; ++spin)
    {
        if (condition())
            return true;
        std::this_thread::yield();
    }
    return false;
}

template <typename Body> void on_cpu(smp::CpuId cpu, Body body)
{
    std::thread thread([&] {
        const smp::ScopedHostedCpu scoped(cpu);
        body();
    });
    thread.join();
}

// The owner pins under the kernel lock, runs without it and commits its own
// result when nothing drained it.
void check_owner_commit()
{
    on_cpu(1, [] {
        const auto before = commits.load();
        smp::bkl_lock();
        smp::enter_section(7, 70);
        const auto &record = smp::section_records[1];
        expect(record.load() == smp::SectionState::working && record.pid == 7 && record.tid == 70,
               "the pin did not publish the record");
        expect(smp::in_section() && !smp::kernel_lock.owned_by_current_cpu() &&
                   (smp::cpu_mode(1) & smp::mode_bkl_held) == 0,
               "a section kept the kernel lock or missed SECTION");
        if constexpr (smp::lock_debug_enabled)
            expect(smp::lock_debug_depth(1) == 0, "a section recorded a hold");
        expect(!smp::require_drained(7, "model owner") && smp::require_drained(8, "model other") &&
                   smp::section_cpus[1].drain_asserts.load() == 1,
               "require_drained did not see the working section");
        smp::leave_section(41);
        expect(smp::kernel_lock.owned_by_current_cpu() && !smp::in_section(), "leave_section did not relock");
        expect(commits.load() == before + 1 && last_commit.record == &record && last_commit.result == 41 &&
                   last_commit.state == smp::SectionState::committed && last_commit.cpu == 1 && last_commit.locked,
               "the owner did not commit its result under the kernel lock");
        expect(record.load() == smp::SectionState::idle && smp::section_cpus[1].handoffs.load() == 0 &&
                   smp::section_cpus[1].completed.load() == 1 && smp::section_cpus[1].sections.load() == 1,
               "owner commit accounting");
        smp::bkl_unlock();
    });
}

// A drain for the section's process waits while it works and commits it once
// it returns; the owner then finds its result committed (a handoff).
void check_drain_handoff()
{
    std::atomic<bool> pinned{false}, finish{false}, drained{false}, draining{false};
    const auto before = commits.load();
    const auto drains = total(&smp::SectionCpu::drains);
    const auto waits = total(&smp::SectionCpu::drain_waits);
    std::thread owner([&] {
        const smp::ScopedHostedCpu scoped(2);
        smp::bkl_lock();
        smp::enter_section(9, 90);
        pinned.store(true);
        wait_until(finish);
        smp::leave_section(-4);
        smp::bkl_unlock();
    });
    wait_until(pinned);
    std::thread drainer([&] {
        const smp::ScopedHostedCpu scoped(0);
        smp::bkl_lock();
        draining.store(true);
        smp::drain(9);
        drained.store(true);
        // Still holding the lock: the owner cannot have taken it back.
        expect(smp::section_records[2].load() == smp::SectionState::committed, "drain left the result uncommitted");
        smp::bkl_unlock();
    });
    wait_until(draining);
    expect(eventually([] { return (smp::cpu_mode(0) & smp::mode_spinning) != 0; }),
           "a waiting drainer was not marked SPINNING");
    expect(!drained.load() && commits.load() == before, "drain did not wait for the working section");
    finish.store(true);
    drainer.join();
    owner.join();
    expect(drained.load() && commits.load() == before + 1 && last_commit.record == &smp::section_records[2] &&
               last_commit.result == -4 && last_commit.cpu == 0 && last_commit.locked,
           "the drainer did not commit the returning section under the kernel lock");
    expect(smp::section_cpus[2].handoffs.load() == 1 && smp::section_records[2].load() == smp::SectionState::idle,
           "the owner did not count the handoff");
    expect(total(&smp::SectionCpu::drains) == drains + 1 && total(&smp::SectionCpu::drain_waits) == waits + 1,
           "drain accounting");
}

// drain(P) ignores another process's section; drain_all waits for it.
void check_drain_scope()
{
    std::atomic<bool> pinned{false}, finish{false}, all_done{false}, all_started{false};
    const auto before = commits.load();
    std::thread owner([&] {
        const smp::ScopedHostedCpu scoped(3);
        smp::bkl_lock();
        smp::enter_section(11, 110);
        pinned.store(true);
        wait_until(finish);
        smp::leave_section(5);
        smp::bkl_unlock();
    });
    wait_until(pinned);
    on_cpu(0, [] {
        smp::bkl_lock();
        smp::drain(12);
        smp::bkl_unlock();
    });
    expect(smp::section_records[3].load() == smp::SectionState::working && commits.load() == before,
           "drain of another process waited or committed");
    std::thread drainer([&] {
        const smp::ScopedHostedCpu scoped(0);
        smp::bkl_lock();
        all_started.store(true);
        smp::drain_all();
        all_done.store(true);
        smp::bkl_unlock();
    });
    wait_until(all_started);
    expect(eventually([] { return (smp::cpu_mode(0) & smp::mode_spinning) != 0; }) && !all_done.load(),
           "drain_all did not wait for the working section");
    finish.store(true);
    drainer.join();
    owner.join();
    expect(all_done.load() && commits.load() == before + 1 && last_commit.cpu == 0 && last_commit.result == 5,
           "drain_all did not commit the returning section");
}

// A section that never waited samples the others: a CPU holding the kernel
// lock in a stretch marked as work is productive, and the release of such a
// stretch moves its `done`. An idle scan (a stretch never marked) is neither.
void check_evidence()
{
    {
        std::atomic<bool> pinned{false}, scanned{false}, finish{false};
        const auto concurrent = total(&smp::SectionCpu::concurrent);
        const auto witnessed = total(&smp::SectionCpu::witnessed);
        std::thread owner([&] {
            const smp::ScopedHostedCpu scoped(10);
            smp::bkl_lock();
            smp::enter_section(19, 190);
            pinned.store(true);
            wait_until(finish);
            smp::leave_section(0);
            smp::bkl_unlock();
        });
        wait_until(pinned);
        std::thread idle([&] {
            const smp::ScopedHostedCpu scoped(11);
            smp::bkl_lock(); // An empty scan...
            smp::bkl_unlock();
            smp::bkl_lock(); // ...and another one in progress at the sample.
            scanned.store(true);
            wait_until(finish);
            expect(eventually([] { return smp::section_records[10].load() != smp::SectionState::working; }),
                   "the section did not end");
            smp::bkl_unlock();
        });
        wait_until(scanned);
        expect(!smp::productive(11), "a CPU scanning under the kernel lock counted as productive");
        finish.store(true);
        idle.join();
        owner.join();
        expect(total(&smp::SectionCpu::concurrent) == concurrent && total(&smp::SectionCpu::witnessed) == witnessed &&
                   smp::section_cpus[10].max_parallel.load() == 1 && smp::section_cpus[10].spun.load() == 0,
               "idle kernel-lock stretches counted as overlap");
        expect(smp::bkl_cpus[11].done.load() == 0 && smp::bkl_cpus[10].done.load() == 2,
               "done counted an idle stretch or missed the section and its commit");
    }
    std::atomic<bool> pinned{false}, worked{false}, finish{false};
    const auto concurrent = total(&smp::SectionCpu::concurrent);
    const auto witnessed = total(&smp::SectionCpu::witnessed);
    std::thread owner([&] {
        const smp::ScopedHostedCpu scoped(4);
        smp::bkl_lock();
        smp::enter_section(13, 130);
        pinned.store(true);
        wait_until(finish);
        smp::leave_section(0);
        smp::bkl_unlock();
    });
    wait_until(pinned);
    std::thread other([&] {
        const smp::ScopedHostedCpu scoped(5);
        smp::bkl_lock(); // A whole stretch of work during the section...
        smp::bkl_mark_work();
        smp::bkl_unlock();
        smp::bkl_lock(); // ...and productive at the sample.
        smp::bkl_mark_work();
        worked.store(true);
        wait_until(finish);
        expect(eventually([] { return smp::section_records[4].load() != smp::SectionState::working; }),
               "the section did not end");
        // Release so the owner can relock.
        smp::bkl_unlock();
    });
    wait_until(worked);
    finish.store(true);
    other.join();
    owner.join();
    expect(total(&smp::SectionCpu::concurrent) == concurrent + 1 &&
               total(&smp::SectionCpu::witnessed) == witnessed + 1 &&
               smp::section_cpus[4].max_parallel.load() == 2 && smp::section_cpus[4].spun.load() == 0,
           "overlap evidence of a non-spinning section");

    // A section that waited for a lock counts as spun and samples nothing.
    smp::RankedSpinLock<smp::LockRank::ofd_position> fine;
    std::atomic<bool> holding{false}, release{false}, spun_pinned{false};
    std::thread holder([&] {
        const smp::ScopedHostedCpu scoped(6);
        fine.lock();
        holding.store(true);
        wait_until(release);
        fine.unlock();
    });
    wait_until(holding);
    std::thread spinner([&] {
        const smp::ScopedHostedCpu scoped(7);
        smp::bkl_lock();
        smp::enter_section(15, 150);
        spun_pinned.store(true);
        fine.lock(); // Waits for CPU 6.
        fine.unlock();
        smp::leave_section(0);
        smp::bkl_unlock();
    });
    wait_until(spun_pinned);
    expect(eventually([] { return (smp::cpu_mode(7) & smp::mode_spinning) != 0; }),
           "a section waiting for a fine lock was not marked SPINNING");
    expect(!smp::productive(7), "a spinning section counted as productive");
    release.store(true);
    holder.join();
    spinner.join();
    expect(smp::section_cpus[7].spun.load() == 1 && smp::section_cpus[7].concurrent.load() == 0 &&
               smp::section_cpus[7].witnessed.load() == 0,
           "a section that waited sampled evidence");
}

// require_exclusive: refused in a section; once armed, refused without the
// kernel lock. Depth assertions where OK_LOCK_DEBUG records holds.
void check_assertions()
{
    on_cpu(8, [] {
        auto &self = smp::section_cpus[8];
        expect(smp::require_exclusive("model unarmed") && self.exclusive_asserts.load() == 0,
               "unarmed require_exclusive without the lock failed");
        smp::arm_exclusive();
        expect(!smp::require_exclusive("model armed") && self.exclusive_asserts.load() == 1,
               "armed require_exclusive without the lock passed");
        expect(std::string_view{self.first_site.load()} == "model armed", "first assertion site not kept");
        smp::bkl_lock();
        expect(smp::require_exclusive("model locked") && self.exclusive_asserts.load() == 1,
               "require_exclusive under the lock failed");
        smp::enter_section(17, 170);
        expect(!smp::require_exclusive("model section") && self.exclusive_asserts.load() == 2,
               "require_exclusive inside a section passed");
        expect(std::string_view{self.first_site.load()} == "model armed", "a later site replaced the first");
        smp::leave_section(0);
        smp::bkl_unlock();
        smp::exclusive_armed.store(false);

        const auto depth = self.depth_asserts.load();
        smp::drain(1); // Without the kernel lock.
        smp::bkl_lock();
        smp::RankedSpinLock<smp::LockRank::console> extra;
        extra.lock();
        smp::drain_all(); // With a second hold.
        extra.unlock();
        smp::drain_all();
        smp::bkl_unlock();
        expect(self.depth_asserts.load() == depth + (smp::lock_debug_enabled ? 2 : 0),
               "drain depth assertions");
    });
}

void check_family_counters()
{
    on_cpu(9, [] {
        smp::count_section_family(smp::SectionFamily::fsync);
        smp::count_section_decline(smp::SectionDecline::append);
        const auto &self = smp::section_cpus[9];
        expect(self.families[static_cast<usize>(smp::SectionFamily::fsync)].load() == 1 &&
                   self.declines[static_cast<usize>(smp::SectionDecline::append)].load() == 1 &&
                   self.declined.load() == 1,
               "family and decline counters");
        expect(smp::section_family_names[static_cast<usize>(smp::SectionFamily::fdatasync)] == "fdatasync" &&
                   smp::section_decline_names[static_cast<usize>(smp::SectionDecline::shutdown)] == "shutdown" &&
                   smp::section_decline_names[static_cast<usize>(smp::SectionDecline::personality)] == "personality",
               "report names out of order");
        expect(!smp::section_admission_open(), "admission open before the runtime opened it");
        smp::open_section_admission();
        expect(smp::section_admission_open(), "admission did not open");
        smp::close_section_admission();
        expect(!smp::section_admission_open(), "admission did not close");
    });
}

} // namespace

int main()
{
    smp::section_commit_handler = record_commit;
    check_owner_commit();
    check_drain_handoff();
    check_drain_scope();
    check_evidence();
    check_assertions();
    check_family_counters();
    expect(total(&smp::SectionCpu::section_asserts) == 0 && total(&smp::SectionCpu::drain_asserts) == 1,
           "protocol counted a section assertion or an unexpected drain assertion");
    if constexpr (smp::lock_debug_enabled)
    {
        u64 violations = 0;
        for (smp::CpuId cpu = 0; cpu < smp::max_cpus; ++cpu)
            violations += smp::lock_violation_count(cpu);
        expect(violations == 0, "the protocol broke lock order");
    }
    std::printf("OK_KERNEL_SECTION_PROTOCOL scope=model mode=%s checks=%d\n",
                smp::lock_debug_enabled ? "debug" : "release", checks);
    return 0;
}
