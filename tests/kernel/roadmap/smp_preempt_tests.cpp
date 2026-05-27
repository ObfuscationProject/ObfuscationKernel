#include "roadmap_tests.hpp"

#include "ok/interrupt/interrupt.hpp"
#include "ok/smp/preempt.hpp"

#include <array>

namespace ok
{
namespace
{

struct InterruptTestContext
{
    interrupt::InterruptDispatcher *dispatcher{nullptr};
    usize divide_by_zero{0};
    usize page_fault{0};
    usize syscall{0};
    usize nested_entries{0};
    usize max_nested_depth{0};
};

Status nested_irq_callback(void *raw, arch::TrapFrame &)
{
    auto *context = static_cast<InterruptTestContext *>(raw);
    ++context->nested_entries;
    if (context->nested_entries > context->max_nested_depth)
    {
        context->max_nested_depth = context->nested_entries;
    }
    --context->nested_entries;
    return Status::success();
}

Status page_fault_callback(void *raw, arch::TrapFrame &)
{
    auto *context = static_cast<InterruptTestContext *>(raw);
    ++context->page_fault;
    ++context->nested_entries;
    if (context->nested_entries > context->max_nested_depth)
    {
        context->max_nested_depth = context->nested_entries;
    }
    arch::TrapFrame nested{.vector = 33};
    if (auto status = context->dispatcher->dispatch(nested); !status.ok())
    {
        return status;
    }
    --context->nested_entries;
    return Status::success();
}

Status test_smp_topology_and_per_cpu_scheduling(Kernel &kernel)
{
    smp::CpuTopology topology;
    if (topology.initialize(0).code() != StatusCode::invalid_argument)
    {
        return Status::fault("SMP accepted zero CPUs");
    }
    if (auto status = topology.initialize(4); !status.ok())
    {
        return status;
    }
    for (smp::CpuId cpu = 0; cpu < 4; ++cpu)
    {
        if (cpu != 0)
        {
            if (auto status = topology.mark_starting(cpu); !status.ok())
            {
                return status;
            }
            const auto *starting = topology.cpu(cpu);
            if (starting == nullptr || starting->state != smp::CpuState::starting)
            {
                return Status::fault("AP startup state validation failed");
            }
        }
        if (auto status = topology.mark_online(cpu); !status.ok())
        {
            return status;
        }
        if (auto status = topology.record_schedule(cpu); !status.ok())
        {
            return status;
        }
    }
    if (topology.online_count() != 4 || topology.mark_online(99).code() != StatusCode::invalid_argument)
    {
        return Status::fault("SMP online validation failed");
    }

    smp::PerCpu<uptr> stacks;
    smp::PerCpu<usize> cpu_data;
    for (smp::CpuId cpu = 0; cpu < 4; ++cpu)
    {
        stacks.get(cpu) = 0x800000 + static_cast<uptr>(cpu) * 0x4000;
        cpu_data.get(cpu) = static_cast<usize>(cpu + 1) * 17;
    }
    for (smp::CpuId cpu = 1; cpu < 4; ++cpu)
    {
        if (stacks.get(cpu) == stacks.get(cpu - 1) || cpu_data.get(cpu) == cpu_data.get(cpu - 1))
        {
            return Status::fault("per-CPU storage uniqueness validation failed");
        }
    }

    sched::Scheduler scheduler;
    if (auto status = scheduler.configure_cpus(4); !status.ok())
    {
        return status;
    }
    auto &ops = arch::arch_operations(kernel.arch().architecture());
    for (usize i = 0; i < 4; ++i)
    {
        auto process = scheduler.create_process("smp-thread", ops.make_kernel_context(0x1000 + i, 0x8000 + i));
        if (!process)
        {
            return process.status();
        }
        if (auto status = scheduler.set_runnable(process.value()); !status.ok())
        {
            return status;
        }
    }
    for (smp::CpuId cpu = 0; cpu < 4; ++cpu)
    {
        auto selected = scheduler.schedule_next_on_cpu(cpu);
        if (!selected || scheduler.current_pid(cpu) == 0)
        {
            return Status::fault("per-CPU scheduler state validation failed");
        }
        for (smp::CpuId previous = 0; previous < cpu; ++previous)
        {
            if (scheduler.current_pid(previous) == scheduler.current_pid(cpu))
            {
                return Status::fault("per-CPU scheduler placed one single-thread process on multiple CPUs");
            }
        }
    }

    std::array<u64, smp::max_cpus> dispatches_before{};
    for (usize cpu = 0; cpu < kernel.scheduler().cpu_count(); ++cpu)
    {
        dispatches_before[cpu] = kernel.scheduler().cpu_stats(static_cast<smp::CpuId>(cpu)).dispatches;
    }
    for (usize i = 0; i < 3; ++i)
    {
        if (auto status = kernel.tick(); !status.ok())
        {
            return status;
        }
    }
    for (usize cpu = 1; cpu < kernel.scheduler().cpu_count(); ++cpu)
    {
        if (kernel.scheduler().cpu_stats(static_cast<smp::CpuId>(cpu)).dispatches <= dispatches_before[cpu])
        {
            return Status::fault("kernel scheduler tick did not dispatch work on secondary CPUs");
        }
    }

    sched::Scheduler advanced;
    if (auto status = advanced.configure_cpus(2); !status.ok())
    {
        return status;
    }
    auto low = advanced.create_process("low-prio", ops.make_kernel_context(0x5000, 0xc000));
    auto high = advanced.create_process("high-prio", ops.make_kernel_context(0x6000, 0xd000));
    if (!low || !high)
    {
        return Status::fault("advanced scheduler process creation failed");
    }
    if (auto status = advanced.set_priority(low.value(), 4); !status.ok())
    {
        return status;
    }
    if (auto status = advanced.set_priority(high.value(), 24); !status.ok())
    {
        return status;
    }
    if (auto status = advanced.set_cpu_affinity(high.value(), 0x1); !status.ok())
    {
        return status;
    }
    if (auto status = advanced.set_runnable(low.value()); !status.ok())
    {
        return status;
    }
    if (auto status = advanced.set_runnable(high.value()); !status.ok())
    {
        return status;
    }
    auto extra_thread = advanced.create_thread(high.value(), ops.make_kernel_context(0x6100, 0xd800));
    if (!extra_thread)
    {
        return extra_thread.status();
    }
    auto high_pick = advanced.schedule_next_on_cpu(0);
    if (!high_pick || high_pick.value() != high.value() || advanced.current_tid(0) == 0 ||
        advanced.cpu_usage_percent(0) == 0)
    {
        return Status::fault("priority scheduler did not select high priority multi-thread process");
    }
    auto low_pick = advanced.schedule_next_on_cpu(1);
    if (!low_pick || low_pick.value() != low.value())
    {
        return Status::fault("CPU affinity did not keep high priority process off CPU1");
    }
    auto module = advanced.spawn(sched::ScheduleRequest{
        .name = "worker:external",
        .initial_context = ops.make_kernel_context(0x7000, 0xe000),
        .priority = 25,
        .cpu_affinity_mask = 0x2,
        .credentials = user::kernel_credentials(),
        .background = true,
    });
    if (!module)
    {
        return module.status();
    }
    const auto *module_process = advanced.find(module.value());
    if (module_process == nullptr || !module_process->background() || module_process->priority() != 25 ||
        module_process->cpu_affinity_mask() != 0x2)
    {
        return Status::fault("generic scheduler spawn interface did not apply process policy");
    }

    sched::Scheduler accounting;
    if (auto status = accounting.configure_cpus(1); !status.ok())
    {
        return status;
    }
    auto idle = accounting.create_process("idle", ops.make_kernel_context(0x8000, 0xf000));
    if (!idle)
    {
        return idle.status();
    }
    if (auto status = accounting.set_runnable(idle.value()); !status.ok())
    {
        return status;
    }
    auto passive = accounting.spawn(sched::ScheduleRequest{
        .name = "drv:passive",
        .initial_context = ops.make_kernel_context(0x9000, 0x10000),
        .priority = sched::scheduler_default_priority,
        .cpu_affinity_mask = sched::cpu_affinity_any,
        .credentials = user::kernel_credentials(),
        .background = true,
        .cpu_accounting = sched::ProcessCpuAccounting::passive,
    });
    if (!passive)
    {
        return passive.status();
    }
    for (usize i = 0; i < 4; ++i)
    {
        if (!accounting.schedule_next())
        {
            return Status::fault("passive CPU accounting scheduler did not dispatch");
        }
    }
    if (accounting.cpu_usage_percent(0) != 0 || accounting.process_usage_percent(idle.value()) != 0 ||
        accounting.process_usage_percent(passive.value()) != 0)
    {
        return Status::fault("idle or passive kernel daemon was counted as CPU work");
    }
    auto active = accounting.create_process("active", ops.make_kernel_context(0xa000, 0x11000));
    if (!active)
    {
        return active.status();
    }
    if (auto status = accounting.set_runnable(active.value()); !status.ok())
    {
        return status;
    }
    for (usize i = 0; i < 4; ++i)
    {
        if (!accounting.schedule_next())
        {
            return Status::fault("active CPU accounting scheduler did not dispatch");
        }
    }
    if (accounting.cpu_usage_percent(0) == 0 || accounting.process_usage_percent(active.value()) == 0 ||
        accounting.process_usage_percent(passive.value()) != 0)
    {
        return Status::fault("active CPU accounting did not isolate passive daemon usage");
    }

    return Status::success();
}

Status test_interrupt_paths(Kernel &kernel)
{
    interrupt::SimulatedInterruptController controller;
    if (controller.enable(32).code() != StatusCode::not_initialized)
    {
        return Status::fault("interrupt controller accepted enable before initialize");
    }
    if (auto status = controller.initialize(); !status.ok())
    {
        return status;
    }
    if (auto status = controller.enable(32); !status.ok())
    {
        return status;
    }
    arch::TrapFrame timer{.vector = 32};
    const auto before = kernel.interrupts().handled_count(32);
    if (auto status = kernel.interrupts().dispatch(timer); !status.ok())
    {
        return status;
    }
    if (!controller.enabled(32) || kernel.interrupts().handled_count(32) != before + 1 ||
        !kernel.interrupts().has_handler(32))
    {
        return Status::fault("timer IRQ validation failed");
    }
    if (auto status = controller.disable(32); !status.ok())
    {
        return status;
    }
    if (controller.enabled(32))
    {
        return Status::fault("interrupt controller disable validation failed");
    }

    interrupt::InterruptDispatcher dispatcher;
    InterruptTestContext context{.dispatcher = &dispatcher};
    if (auto status = dispatcher.register_callback(0, "divide-by-zero", &context,
                                                   [](void *raw, arch::TrapFrame &) {
                                                       ++static_cast<InterruptTestContext *>(raw)->divide_by_zero;
                                                       return Status::success();
                                                   });
        !status.ok())
    {
        return status;
    }
    if (auto status = dispatcher.register_callback(14, "page-fault", &context, page_fault_callback); !status.ok())
    {
        return status;
    }
    if (auto status = dispatcher.register_callback(33, "nested", &context, nested_irq_callback); !status.ok())
    {
        return status;
    }
    if (auto status = dispatcher.register_callback(128, "syscall", &context,
                                                   [](void *raw, arch::TrapFrame &) {
                                                       ++static_cast<InterruptTestContext *>(raw)->syscall;
                                                       return Status::success();
                                                   });
        !status.ok())
    {
        return status;
    }

    arch::TrapFrame divide{.vector = 0};
    arch::TrapFrame fault{.vector = 14};
    arch::TrapFrame syscall{.vector = 128};
    if (!dispatcher.dispatch(divide).ok() || !dispatcher.dispatch(fault).ok() || !dispatcher.dispatch(syscall).ok())
    {
        return Status::fault("controlled interrupt dispatch failed");
    }
    if (context.divide_by_zero != 1 || context.page_fault != 1 || context.syscall != 1 ||
        dispatcher.handled_count(33) != 1 || context.nested_entries != 0 || context.max_nested_depth != 2)
    {
        return Status::fault("exception/syscall/nested interrupt validation failed");
    }

    return Status::success();
}

Status test_preemption()
{
    auto &ops = arch::arch_operations(arch::configured_architecture());

    sched::Scheduler idle_scheduler;
    if (auto status = idle_scheduler.configure_cpus(1); !status.ok())
    {
        return status;
    }
    auto idle = idle_scheduler.create_process("idle", ops.make_kernel_context(0x1000, 0x8000));
    if (!idle)
    {
        return idle.status();
    }
    if (auto status = idle_scheduler.set_runnable(idle.value()); !status.ok())
    {
        return status;
    }
    auto idle_selected = idle_scheduler.schedule_next();
    if (!idle_selected || idle_selected.value() != idle.value())
    {
        return Status::fault("idle thread scheduling validation failed");
    }

    sched::Scheduler scheduler;
    if (auto status = scheduler.configure_cpus(1); !status.ok())
    {
        return status;
    }
    auto first = scheduler.create_process("preempt-a", ops.make_kernel_context(0x2000, 0x9000));
    auto second = scheduler.create_process("preempt-b", ops.make_kernel_context(0x3000, 0xa000));
    auto blocked = scheduler.create_process("blocked", ops.make_kernel_context(0x4000, 0xb000));
    if (!first || !second || !blocked)
    {
        return Status::fault("preemption scheduler setup failed");
    }
    if (auto status = scheduler.set_runnable(first.value()); !status.ok())
    {
        return status;
    }
    if (auto status = scheduler.set_runnable(second.value()); !status.ok())
    {
        return status;
    }

    smp::PreemptionController preempt;
    if (preempt.initialize(0).code() != StatusCode::invalid_argument)
    {
        return Status::fault("preemption accepted zero CPUs");
    }
    if (auto status = preempt.initialize(1); !status.ok())
    {
        return status;
    }
    if (auto status = preempt.tick(0, scheduler); !status.ok())
    {
        return status;
    }
    const auto first_pid = scheduler.current_pid();
    if (first_pid == 0 || first_pid == blocked.value())
    {
        return Status::fault("first preemption tick selected an invalid process");
    }
    if (auto status = preempt.tick(0, scheduler); !status.ok())
    {
        return status;
    }
    const auto second_pid = scheduler.current_pid();
    if (second_pid == 0 || second_pid == first_pid || second_pid == blocked.value() || preempt.switches(0) != 2)
    {
        return Status::fault("preemption did not alternate runnable processes");
    }

    preempt.disable(0);
    if (auto status = preempt.tick(0, scheduler); !status.ok())
    {
        return status;
    }
    if (scheduler.current_pid() != second_pid || preempt.switches(0) != 2)
    {
        return Status::fault("preemption disabled region switched process");
    }
    if (auto status = preempt.enable(0); !status.ok())
    {
        return status;
    }
    if (!preempt.preemptible(0))
    {
        return Status::fault("preemption did not re-enable");
    }

    if (auto status = preempt.sleep_current(0, 2); !status.ok())
    {
        return status;
    }
    if (preempt.preemptible(0))
    {
        return Status::fault("sleeping CPU was preemptible");
    }
    if (auto status = preempt.wake_sleepers(preempt.ticks(0) + 2); !status.ok())
    {
        return status;
    }
    if (!preempt.preemptible(0))
    {
        return Status::fault("sleeping CPU did not wake");
    }

    return Status::success();
}

} // namespace

Status run_smp_irq_preempt_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    if (auto status = test_smp_topology_and_per_cpu_scheduling(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_interrupt_paths(kernel); !status.ok())
    {
        return status;
    }
    if (auto status = test_preemption(); !status.ok())
    {
        return status;
    }

    report.smp_roadmap = true;
    report.irq_roadmap = true;
    report.preempt = true;
    return Status::success();
}

} // namespace ok
