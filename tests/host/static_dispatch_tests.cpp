#include "ok/arch/arch.hpp"
#include "ok/sched/process.hpp"
#include "ok/sched/scheduler.hpp"

#include <type_traits>
#include <memory>

namespace
{

using namespace ok;

template <typename Condition> void check(Condition &&condition)
{
    if (!static_cast<bool>(condition))
    {
        __builtin_trap();
    }
}

// Exercise two genuinely different policy instantiations against the same
// scheduling mechanism. This policy deliberately chooses the newest task.
struct NewestRunnablePolicy
{
    usize selections{0};

    Result<sched::ProcessId> pick_next(std::span<const sched::ProcessControlBlock> processes, sched::ProcessId current,
                                       smp::CpuId cpu)
    {
        ++selections;
        for (usize i = processes.size(); i != 0; --i)
        {
            const auto &process = processes[i - 1];
            if (process.can_run_on(cpu) &&
                (process.state() == sched::ProcessState::runnable || process.pid() == current))
            {
                return process.pid();
            }
        }
        return Status::would_block("no eligible task");
    }
};

struct InvalidPolicy
{
    void pick_next();
};

static_assert(sched::SchedulerPolicy<NewestRunnablePolicy>);
static_assert(!sched::SchedulerPolicy<InvalidPolicy>);
static_assert(!std::is_polymorphic_v<sched::RoundRobinPolicy>);
static_assert(!std::is_polymorphic_v<sched::BasicScheduler<NewestRunnablePolicy>>);
static_assert(!std::is_copy_constructible_v<sched::BasicScheduler<NewestRunnablePolicy>>);
static_assert(!std::is_copy_assignable_v<sched::BasicScheduler<NewestRunnablePolicy>>);
static_assert(std::is_same_v<sched::Scheduler, sched::BasicScheduler<sched::RoundRobinPolicy>>);

struct RecordingInstructions
{
    static inline u32 calls{0};
    static u64 read_cycle_counter() noexcept
    {
        calls |= 1;
        return 42;
    }
    static void memory_fence() noexcept
    {
        calls |= 2;
    }
    static void enable_interrupts() noexcept
    {
        calls |= 4;
    }
    static void disable_interrupts() noexcept
    {
        calls |= 8;
    }
    static void halt() noexcept
    {
        calls |= 16;
    }
};

static_assert(arch::ArchitectureInstructions<RecordingInstructions>);

template <arch::Architecture A> constexpr bool valid_profile()
{
    constexpr arch::ArchitectureOperations<A> operations;
    constexpr auto kernel = operations.make_kernel_context(0x1234, 0x8000);
    constexpr auto user = operations.make_user_context({0x400000, 0x9000, 7});
    static_assert(!std::is_polymorphic_v<decltype(operations)>);
    return operations.architecture() == A && operations.page_size() == 4096 &&
           operations.name() == arch::to_string(A) && operations.register_count() > 0 && kernel.architecture == A &&
           kernel.program_counter == 0x1234 && kernel.stack_pointer == 0x8000 &&
           kernel.mode == arch::PrivilegeMode::kernel && user.architecture == A && user.program_counter == 0x400000 &&
           user.stack_pointer == 0x9000 && user.registers[0] == 7 && user.mode == arch::PrivilegeMode::user;
}

static_assert(valid_profile<arch::Architecture::i386>());
static_assert(valid_profile<arch::Architecture::x86_64>());
static_assert(valid_profile<arch::Architecture::aarch64>());
static_assert(valid_profile<arch::Architecture::arm32>());
static_assert(valid_profile<arch::Architecture::rv64>());
static_assert(valid_profile<arch::Architecture::rv32>());
static_assert(valid_profile<arch::Architecture::loongarch64>());
static_assert(valid_profile<arch::Architecture::mips>());
static_assert(valid_profile<arch::Architecture::mips64>());
static_assert(valid_profile<arch::Architecture::ppc>());
static_assert(arch::ArchitectureOperations<arch::Architecture::mips>{}.endianness() == arch::Endianness::big);
static_assert(arch::ArchitectureOperations<arch::Architecture::x86_64>{}.endianness() == arch::Endianness::little);

void test_architecture_dispatch()
{
    arch::ArchitectureOperations<arch::Architecture::x86_64, RecordingInstructions> x86;
    arch::ArchitectureOperations<arch::Architecture::aarch64, RecordingInstructions> arm;
    check(x86.architecture() != arm.architecture());
    check(x86.read_cycle_counter() == 42);
    x86.memory_fence();
    arm.enable_interrupts();
    arm.disable_interrupts();
    arm.halt();
    check(RecordingInstructions::calls == 31);

    auto &native = arch::configured_arch_operations();
    check(native.architecture() == arch::configured_architecture());
    check(&native == &arch::arch_operations(arch::configured_architecture()));
    check(native.read_cycle_counter() != 0);
    native.memory_fence();
    const auto other = arch::configured_architecture() == arch::Architecture::x86_64 ? arch::Architecture::aarch64
                                                                                     : arch::Architecture::x86_64;
    check(arch::validate_architecture(other).code() == StatusCode::unsupported);
    check(arch::validate_architecture(arch::configured_architecture()).ok());
}

void test_reject_foreign_process_target()
{
    static sched::Scheduler process_owner;
    sched::ProcessManager processes(process_owner);
    const auto other = arch::configured_architecture() == arch::Architecture::x86_64 ? arch::Architecture::aarch64
                                                                                     : arch::Architecture::x86_64;
    check(processes.create_user_process("foreign", {}, other).status().code() == StatusCode::unsupported);
    check(processes.process_count() == 0);
    auto kernel_thread = processes.create_kernel_thread("kernel", arch::ArchOperations{}.make_kernel_context(1, 2));
    check(kernel_thread);
    check(processes.execve(kernel_thread.value(), {}, other).code() == StatusCode::unsupported);
    check(processes.find(kernel_thread.value())->threads()[0].context.mode == arch::PrivilegeMode::kernel);
}

template <sched::SchedulerPolicy Policy>
sched::ProcessId add_task(sched::BasicScheduler<Policy> &scheduler, std::string_view name)
{
    auto created = scheduler.create_process(name, arch::ArchOperations{}.make_kernel_context(0x1000, 0x8000));
    check(created);
    check(scheduler.set_runnable(created.value()).ok());
    return created.value();
}

void test_default_policy()
{
    auto owner = std::make_unique<sched::Scheduler>();
    auto &scheduler = *owner;
    check(scheduler.configure_cpus(2).ok());
    const auto first = add_task(scheduler, "first");
    const auto second = add_task(scheduler, "second");
    check(scheduler.schedule_next().value() == first);
    check(scheduler.schedule_next().value() == second);
    check(scheduler.set_priority(first, 24).ok());
    check(scheduler.set_cpu_affinity(first, 1).ok());
    check(scheduler.schedule_next_on_cpu(0).value() == first);
    check(scheduler.schedule_next_on_cpu(1).value() == second);
    check(scheduler.current_tid(0) != scheduler.current_tid(1));
    check(scheduler.cpu_stats(0).dispatches == 3);
    check(scheduler.cpu_stats(1).dispatches == 1);
    check(scheduler.block_process(first).ok());
    check(scheduler.current_pid(0) == 0);
    check(scheduler.schedule_next_on_cpu(2).status().code() == StatusCode::invalid_argument);
}

void test_alternate_policy_and_ownership()
{
    auto original_owner = std::make_unique<sched::BasicScheduler<NewestRunnablePolicy>>();
    auto independent_owner = std::make_unique<sched::BasicScheduler<NewestRunnablePolicy>>();
    auto &original = *original_owner;
    auto &independent = *independent_owner;
    const auto first = add_task(original, "first");
    const auto second = add_task(original, "second");
    check(original.schedule_next().value() == second);
    check(original.policy().selections == 1);

    check(add_task(independent, "first") == first);
    check(add_task(independent, "second") == second);
    check(independent.schedule_next().value() == second);
    check(independent.policy().selections == 1);
    check(independent.find(first) != original.find(first));
    check(independent.find(second) != original.find(second));
    check(independent.block_process(second).ok());
    check(independent.schedule_next().value() == first);
    check(independent.policy().selections == 2);
    check(original.policy().selections == 1);
    check(original.current_pid() == second);
    check(original.find(second)->state() == sched::ProcessState::running);
    check(independent.schedule_next_on_cpu(1).status().code() == StatusCode::invalid_argument);
    check(independent.policy().selections == 2);
    check(independent.block_process(first).ok());
    check(independent.schedule_next().status().code() == StatusCode::would_block);
    check(independent.current_pid() == 0);
}

} // namespace

int main()
{
    test_architecture_dispatch();
    test_reject_foreign_process_target();
    test_default_policy();
    test_alternate_policy_and_ownership();
}
