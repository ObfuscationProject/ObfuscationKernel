// Actual runtime object-backed waits with observed hosted mapping/copy backends.
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
namespace lu = ok::abi::linux_user;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                                            \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)
namespace mock
{
std::array<std::byte, 8192> memory{};
uptr forbidden{};
usize copies{};
usize outputs{};
usize partial_output{};
uptr last_input{};
usize last_input_size{};
u64 now = 3000000042;
bool clock_fail{};
StatusCode clock_failure = StatusCode::io_error;
usize clock_reads{};
bool realtime_fail = true;
StatusCode realtime_failure = StatusCode::not_initialized;
time::EpochTime epoch{1709210096, 123456789};
usize realtime_reads{};
bool setting_enabled{};
usize setting_calls{};
time::EpochTime last_setting{};
time::MonotonicPoint setting_point{};
Status setting_status{};
usize entries{};
usize create_space_calls{};
usize mapped_pages{};
usize destroy_space_calls{};
hw::UserFrame last_frame{};
} // namespace mock
namespace ok::arch
{
Status initialize_realtime() { return Status::not_initialized("hosted RTC unavailable"); }
Result<time::MonotonicPoint> monotonic_sample()
{
    auto value = monotonic_nanoseconds();
    return value ? Result<time::MonotonicPoint>(time::MonotonicPoint{value.value(), 1})
                 : Result<time::MonotonicPoint>(value.status());
}
Status set_realtime(time::EpochTime epoch, time::MonotonicPoint point)
{
    ++mock::setting_calls;
    if (!mock::setting_enabled)
        return Status::unsupported("this hosted regression does not exercise clock setting");
    if (!mock::setting_status)
        return mock::setting_status;
    mock::last_setting = epoch;
    mock::setting_point = point;
    return Status::success();
}

time::AnchorInfo realtime_info() { return {}; }
Result<time::EpochTime> realtime()
{
    ++mock::realtime_reads;
    if (mock::realtime_fail) return Status{mock::realtime_failure};
    return mock::epoch;
}
Status initialize_clock()
{
    return Status::success();
}
Result<u64> monotonic_nanoseconds()
{
    ++mock::clock_reads;
    return mock::clock_fail ? Result<u64>(Status{mock::clock_failure}) : Result<u64>(mock::now);
}
u64 clock_resolution_ns()
{
    return mock::clock_fail ? 0 : 1;
}
} // namespace ok::arch
namespace ok::arch::execution
{
bool supported() noexcept
{
    return false;
}
Status initialize()
{
    return Status::unsupported();
}
Result<AddressSpace> create_space(MappingView mappings)
{
    ++mock::create_space_calls;
    mock::mapped_pages += mappings.size();
    return AddressSpace{.root = 1, .slot = 0, .generation = 1};
}
Status destroy_space(AddressSpace &space)
{
    ++mock::destroy_space_calls;
    space = {};
    return Status::success();
}
Result<Trap> enter_owned(AddressSpace &space, UserFrame &frame, const KernelEntryStackView &view)
{
    if (!view.valid()) return Status::invalid_argument("hosted entry requires a live lease");
    auto result = enter(space, frame);
    if (result) result.value().kernel_stack_pointer = view.lower(); // Synthetic hosted capture only.
    return result;
}
Result<Trap> enter(AddressSpace &, UserFrame &frame)
{
    ++mock::entries; mock::last_frame=frame;
    return Trap{TrapKind::timer};
}
UserFrame initial_frame(uptr pc, uptr sp)
{
    UserFrame f{};
    f.pc = pc;
    f.sp = sp;
    return f;
}
Syscall syscall(const UserFrame &)
{
    return {};
}
void set_return_value(UserFrame &f, i64 v)
{
    f.registers[0] = static_cast<uptr>(v);
}
bool user_range(const AddressSpace &, uptr address, usize size, bool)
{
    return !size || (address >= user_begin && address - user_begin < mock::memory.size() &&
                     size <= mock::memory.size() - (address - user_begin) &&
                     (!mock::forbidden || address >= mock::forbidden + 1 || size <= mock::forbidden - address));
}
Status copy_from_user(const AddressSpace &s, void *out, uptr address, usize size)
{
    ++mock::copies;
    mock::last_input=address; mock::last_input_size=size;
    if (!user_range(s, address, size, false))
        return Status::fault();
    if (size)
        std::memcpy(out, mock::memory.data() + address - user_begin, size);
    return Status::success();
}
Status copy_to_user(const AddressSpace &s, uptr address, const void *in, usize size)
{
    ++mock::outputs;
    if (!user_range(s, address, size, true))
        return Status::fault();
    if (mock::partial_output)
    {
        std::memcpy(mock::memory.data() + address - user_begin, in, mock::partial_output);
        return Status::fault();
    }
    if (size)
        std::memcpy(mock::memory.data() + address - user_begin, in, size);
    return Status::success();
}
} // namespace ok::arch::execution

using sched::ProcessId;
using sched::ThreadId;
using memory::ObjectHandle;

namespace
{
struct CountedObject
{
    static inline usize constructed{}, destroyed{};
    u64 value;
    explicit CountedObject(u64 number) noexcept : value(number) { ++constructed; }
    ~CountedObject() noexcept { ++destroyed; }
};

void provider_mapping_and_lifetime()
{
    CHECK(native::activate_pool());
    CHECK(native::page_pool.user_count() == 0 && native::page_pool.object_count() == 0);
    CHECK(native::page_pool.kernel_reserve_count() == native::kernel_object_pages);
    CHECK(native::page_pool.user_capacity_count() + native::page_pool.kernel_reserve_count() ==
          native::page_pool.total_count());
    auto lease = native::page_pool.allocate_kernel_page();
    CHECK(lease);
    const auto address = reinterpret_cast<uptr>(lease.value().bytes.data());
    CHECK(!native::page_pool.owns(address) && !native::page_pool.handle(address));
    CHECK(!native::page_pool.pin(address) && !native::page_pool.release(address));
    native::Pages pages;
    CHECK(pages.push_back({hw::user_begin, address, hw::page_read}));
    const auto calls = mock::create_space_calls;
    auto denied = native::create_page_space(pages);
    CHECK(!denied && denied.status().code() == StatusCode::fault && mock::create_space_calls == calls);
    pages[0].flags = 0;
    denied = native::create_page_space(pages);
    CHECK(!denied && denied.status().code() == StatusCode::fault && mock::create_space_calls == calls);
    auto user = native::page_pool.allocate();
    CHECK(user);
    pages.clear();
    CHECK(pages.push_back({hw::user_begin, user.value(), hw::page_read}));
    auto admitted = native::create_page_space(pages);
    CHECK(admitted && mock::create_space_calls == calls + 1 && mock::mapped_pages == 1);
    CHECK(hw::destroy_space(admitted.value()));
    CHECK(native::page_pool.release(user.value()));
    CHECK(native::page_pool.release_kernel_page(lease.value()));
    CHECK(!native::page_pool.release_kernel_page(lease.value()));
    CHECK(native::page_pool.used_count() == 0);

    memory::ObjectPool<CountedObject, native::NativePagePool, 1> objects{native::page_pool, 2};
    auto object = objects.create(42);
    CHECK(object && objects.resolve(object.value()).value()->value == 42);
    CHECK(CountedObject::constructed == 1 && CountedObject::destroyed == 0);
    CHECK(objects.destroy(object.value()));
    CHECK(CountedObject::destroyed == 1 && !objects.resolve(object.value()) && !objects.destroy(object.value()));
    CHECK(CountedObject::destroyed == 1);
    CHECK(objects.trim_empty_pages() && native::page_pool.object_count() == 0);
    auto next = objects.create(43);
    CHECK(next && next.value().generation > object.value().generation);
    CHECK(!objects.resolve(object.value()) && !objects.destroy(object.value()));
    CHECK(objects.destroy(next.value()) && objects.trim_empty_pages());
    CHECK(CountedObject::constructed == 2 && CountedObject::destroyed == 2);
    CHECK(objects.accounting().allocations == 2 && objects.accounting().destructions == 2);
    CHECK(objects.accounting().live_objects == 0 && objects.accounting().backing_pages == 0);
    CHECK(native::page_pool.used_count() == 0 && native::page_pool.allocations() == native::page_pool.releases());
}

struct Owner
{
    ProcessId pid{};
    std::array<ThreadId, sched::max_threads_per_process> tids{};
    i32 read_fd{}, write_fd{};
    u16 read_description{}, write_description{};
};

Owner create_owner(Kernel &kernel, usize threads)
{
    Owner owner;
    auto created = kernel.scheduler().create_user_process("poll-objects", kernel.arch().make_user_context(
        {hw::user_begin, hw::user_end - hw::page_size}));
    CHECK(created);
    owner.pid = created.value();
    auto *process = kernel.scheduler().find(owner.pid);
    CHECK(native::prepare_native_thread(process->threads()[0]));
    owner.tids[0] = process->threads()[0].tid;
    for (usize i = 1; i < threads; ++i)
    {
        auto peer = kernel.scheduler().create_thread_prepared(owner.pid, process->threads()[0].context,
            [](sched::ThreadControlBlock &created) { return native::prepare_native_thread(created); });
        CHECK(peer);
        owner.tids[i] = peer.value();
    }
    CHECK(kernel.scheduler().set_runnable(owner.pid));
    for (auto &thread : process->threads())
    {
        thread.native_active = true;
        thread.native_frame.pc = hw::user_begin + thread.tid * 16;
    }
    CHECK(kernel.posix().activate(owner.pid));
    auto pipe = kernel.posix().pipe(posix::o_NONBLOCK);
    CHECK(pipe);
    owner.read_fd = pipe.value()[0];
    owner.write_fd = pipe.value()[1];
    owner.read_description = process->fd_table().entry(owner.read_fd)->description;
    owner.write_description = process->fd_table().entry(owner.write_fd)->description;
    CHECK(kernel.scheduler().open_description(owner.read_description)->references == 1);
    return owner;
}

ObjectHandle wait(Kernel &kernel, const Owner &owner, usize thread_index)
{
    CHECK(kernel.scheduler().activate_thread(owner.pid, owner.tids[thread_index]));
    std::array<ok_pollfd, 1> descriptors{{{owner.read_fd, OK_POLLIN, 0}}};
    native::PollRequest request{};
    request.descriptors = descriptors;
    request.output = hw::user_begin;
    bool complete = true;
    CHECK(native::begin_poll(kernel, owner.pid, request, complete) == 0 && !complete);
    auto *thread = kernel.scheduler().find_thread(owner.pid, owner.tids[thread_index]);
    CHECK(thread && thread->state == sched::ProcessState::blocked);
    CHECK(thread->wait_registration.active());
    for (const auto &entry : native::poll_waits)
        if (entry.pid == owner.pid && entry.tid == owner.tids[thread_index])
        {
            auto record = native::poll_objects.resolve(entry.object);
            CHECK(record && record.value()->pid == owner.pid && record.value()->tid == owner.tids[thread_index]);
            CHECK(record.value()->ticket == thread->wait_registration.ticket());
            return entry.object;
        }
    CHECK(false);
    return {};
}

void pressure_and_cleanup(Kernel &kernel)
{
    constexpr usize owner_count = 8;
    static_assert(owner_count * sched::max_threads_per_process == sched::max_processes);
    static_assert(native::PollObjects::capacity >= sched::max_processes);
    std::array<uptr, native::NativePagePool::capacity> users{};
    const auto quota = native::page_pool.user_capacity_count();
    for (usize i = 0; i < quota; ++i)
    {
        auto page = native::page_pool.allocate();
        CHECK(page);
        users[i] = page.value();
    }
    const auto denied_user = native::page_pool.allocate();
    CHECK(!denied_user && denied_user.status().code() == StatusCode::no_memory);
    CHECK(native::page_pool.user_count() == quota && native::page_pool.object_count() == 0);
    CHECK(native::page_pool.free_count() == native::kernel_object_pages);
    const auto baseline = native::poll_objects.accounting();
    CHECK(baseline.live_objects == 0 && baseline.backing_pages == 0);
    std::array<Owner, owner_count + 1> owners{};
    std::array<ObjectHandle, sched::max_processes> handles{};
    usize count = 0;
    for (usize i = 0; i < owner_count; ++i)
    {
        owners[i] = create_owner(kernel, sched::max_threads_per_process);
        for (usize thread = 0; thread < sched::max_threads_per_process; ++thread)
            handles[count++] = wait(kernel, owners[i], thread);
        CHECK(kernel.scheduler().open_description(owners[i].read_description)->references == 9);
    }
    CHECK(count == 64 && native::poll_objects.accounting().live_objects == 64);
    CHECK(native::poll_objects.accounting().backing_pages == native::poll_object_pages);
    CHECK(native::page_pool.object_count() == native::poll_object_pages && native::page_pool.free_count() == native::futex_object_pages + native::pipe_read_object_pages);
    CHECK(native::page_pool.user_count() == quota && !native::page_pool.allocate());

    owners[owner_count] = create_owner(kernel, 1);
    const auto &overflow = owners[owner_count];
    CHECK(kernel.scheduler().activate_thread(overflow.pid, overflow.tids[0]));
    auto *thread = kernel.scheduler().current_thread();
    const auto state = thread->state;
    const auto registration = thread->wait_registration.ticket();
    const auto references = kernel.scheduler().open_description(overflow.read_description)->references;
    const auto before = native::poll_objects.accounting();
    const auto outputs = mock::outputs;
    std::array<ok_pollfd, 1> descriptors{{{overflow.read_fd, OK_POLLIN, 0}}};
    native::PollRequest request{};
    request.descriptors = descriptors;
    request.output = hw::user_begin;
    bool complete = true;
    CHECK(native::begin_poll(kernel, overflow.pid, request, complete) == -12 && complete);
    CHECK(thread->state == state && thread->wait_registration.ticket() == registration &&
          !thread->wait_registration.active());
    CHECK(kernel.scheduler().open_description(overflow.read_description)->references == references);
    CHECK(mock::outputs == outputs && native::poll_objects.accounting().allocations == before.allocations);
    CHECK(native::poll_objects.accounting().live_objects == 64 && native::page_pool.object_count() == native::poll_object_pages);

    const auto stale = handles[0];
    native::cancel_poll(kernel, stale);
    CHECK(!native::poll_objects.resolve(stale) && !native::poll_objects.destroy(stale));
    CHECK(kernel.scheduler().open_description(owners[0].read_description)->references == 8);
    CHECK(native::poll_objects.accounting().live_objects == 63);
    auto resumed = wait(kernel, overflow, 0);
    CHECK(resumed.generation > stale.generation && !native::poll_objects.resolve(stale));
    CHECK(!native::poll_objects.destroy(stale));
    CHECK(native::poll_objects.accounting().live_objects == 64);
    CHECK(kernel.scheduler().open_description(overflow.read_description)->references == 2);
    CHECK(!native::page_pool.allocate());

    // Process-scoped cancellation drains seven remaining peer registrations,
    // while preserving waits owned by all other processes.
    native::cancel_poll(kernel, owners[0].pid);
    CHECK(kernel.scheduler().open_description(owners[0].read_description)->references == 1);
    CHECK(native::poll_objects.accounting().live_objects == 57);
    for (usize i = 0; i < 8; ++i) CHECK(!native::poll_objects.resolve(handles[i]));
    native::cancel_poll(kernel, owners[0].pid);
    CHECK(native::poll_objects.accounting().live_objects == 57);

    // Closing the last writer completes each of eight independent thread waits.
    const auto ready_outputs = mock::outputs;
    CHECK(kernel.scheduler().close_descriptor(kernel.scheduler().find(owners[1].pid)->resources(), owners[1].write_fd));
    native::started = true;
    CHECK(native::step(kernel));
    CHECK(mock::outputs == ready_outputs + 8);
    CHECK(native::poll_objects.accounting().live_objects == 49);
    CHECK(kernel.scheduler().open_description(owners[1].read_description)->references == 1);
    for (auto tid : owners[1].tids)
        CHECK(kernel.scheduler().find_thread(owners[1].pid, tid)->wait_registration.completion() == sched::WaitCompletion::ready);

    // Raw scheduler exit releases descriptor ownership; the runtime later drops
    // the eight pins even if the PCB is reaped and the vector compacted first.
    CHECK(native::retire_group_stacks(*kernel.scheduler().find(owners[2].pid)));
    CHECK(kernel.scheduler().exit_process(owners[2].pid, 0));
    CHECK(kernel.scheduler().open_description(owners[2].read_description)->references == 8);
    CHECK(native::step(kernel));
    CHECK(!kernel.scheduler().find(owners[2].pid));
    CHECK(!kernel.scheduler().open_description(owners[2].read_description));
    CHECK(native::poll_objects.accounting().live_objects == 41);

    // Exec cancels both surviving and removed TID records; no stale completion
    // copies a result into the replacement address space.
    const auto exec_outputs = mock::outputs;
    sched::MemoryMap memory;
    auto *exec_owner = kernel.scheduler().find(owners[3].pid);
    CHECK(native::replace_entry_stacks(native::entry_stacks, *exec_owner, [&] {
        return kernel.scheduler().replace_image(owners[3].pid, exec_owner->threads()[0].context, memory);
    }));
    CHECK(native::step(kernel));
    CHECK(mock::outputs == exec_outputs);
    CHECK(native::poll_objects.accounting().live_objects == 33);
    CHECK(kernel.scheduler().open_description(owners[3].read_description)->references == 1);

    for (const auto &owner : owners) native::release_process(kernel, owner.pid);
    const auto stacks = native::entry_stacks.accounting();
    CHECK(!stacks.leased_slots && !stacks.active_slots && !stacks.quarantined_slots);
    CHECK(stacks.reservations == stacks.releases);
    CHECK(native::poll_objects.accounting().live_objects == 0);
    CHECK(native::poll_objects.accounting().backing_pages == 0 && native::poll_objects.accounting().quarantined_pages == 0);
    CHECK(native::poll_objects.accounting().allocations - baseline.allocations == 65);
    CHECK(native::poll_objects.accounting().destructions - baseline.destructions == 65);
    CHECK(native::page_pool.object_count() == 0 && native::page_pool.user_count() == quota);
    CHECK(native::page_pool.free_count() == native::kernel_object_pages && !native::page_pool.allocate());
    CHECK(!native::poll_objects.resolve(resumed));
    for (const auto &handle : handles) CHECK(!native::poll_objects.resolve(handle));
    for (const auto &owner : owners)
        if (auto *process = kernel.scheduler().find(owner.pid))
        {
            CHECK(kernel.scheduler().open_description(owner.read_description)->references == 1);
            CHECK(kernel.scheduler().exit_process(process->pid(), 0));
        }
    for (usize i = 0; i < quota; ++i) CHECK(native::page_pool.release(users[i]));
    CHECK(native::page_pool.used_count() == 0 && native::page_pool.free_count() == native::page_pool.total_count());
    CHECK(native::page_pool.allocations() == native::page_pool.releases());
    CHECK(native::runtime_status());
}
} // namespace

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    CHECK(native::entry_stacks.activate(native::entry_stack_arena));
    provider_mapping_and_lifetime();
    pressure_and_cleanup(kernel);
    std::printf("PASS hosted actual-runtime poll objects: registrations=64 overflow=ENOMEM resumed=1 "
                "destructions=65 user_mapping_object_refused=1 backing_pages=%zu guest=0 hardware_isolation=0 smp=0\n",
                native::poll_object_pages);
}
