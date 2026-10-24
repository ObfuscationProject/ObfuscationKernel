#include "../kernel/points/native_pty_checks.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #condition); std::abort(); } } while (false)
int main()
{
    static sched::Scheduler scheduler;
    const auto sentinel = scheduler.create_process("pty-sentinel", {}); CHECK(sentinel);
    CHECK(scheduler.activate(sentinel.value()));
    const auto current = scheduler.current_pid();
    auto *process = scheduler.find(current); CHECK(process);
    const auto pipe = scheduler.create_pipe(process->resources()); CHECK(pipe);
    const auto process_count = scheduler.process_count();
    const auto description_count = scheduler.open_description_count();
    const auto descriptors = process->resources().files.open_count();
    for (usize iteration = 0; iteration < 128; ++iteration)
    {
        const auto status = tests::check_native_pty_ownership_model(scheduler);
        if (!status) { std::fprintf(stderr, "boot model status=%u\n", static_cast<unsigned>(status.code())); std::abort(); }
        CHECK(scheduler.current_pid() == current && scheduler.process_count() == process_count);
        CHECK(scheduler.open_description_count() == description_count);
        process = scheduler.find(current); CHECK(process && process->resources().files.open_count() == descriptors);
        CHECK(process->resources().files.entry(pipe.value()[0])->used && process->resources().files.entry(pipe.value()[1])->used);
    }
    CHECK(scheduler.kill_process(sentinel.value()));
    CHECK(scheduler.open_description_count() == 3 && scheduler.process_count() == 0);
    CHECK(scheduler.pty_audit().live_pairs == 0 && scheduler.pty_audit().live_endpoints == 0);
    CHECK(scheduler.pty_audit().cleared_outside_live_bytes);
    std::printf("NATIVE_PTY_BOOT_MODEL_HOSTED_PASS iterations=128 unrelated_owner_preserved=1 guest=0\n");
}
