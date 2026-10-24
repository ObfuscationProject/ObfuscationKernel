// Private serialized runtime owners. Endpoint generations are never packed into wait keys.
static_assert(sched::private_pty::RawPtyTable<>::queue_capacity == sched::pipe_capacity,
              "PTY staging uses the separately checked native vector leaf capacity");
struct PtyWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    sched::WaitTicket ticket{};
    u64 request{};
    hw::AddressSpace space{};
    memory::KernelEntryStackHandle lease{};
    hw::UserFrame frame{};
    uptr address{};
    usize count{}, committed{};
    i32 fd{}; // Diagnostic only after admission.
    u16 description{sched::invalid_description};
    sched::private_pty::Handle endpoint{};
    PipeVectors vectors{};
    bool writing{};
};
constexpr usize pty_wait_capacity = memory::KernelEntryStackPool::capacity;
using PtyWaitObjectsLayout = memory::ObjectPool<PtyWait, NativePagePool, 1>;
constexpr usize pty_wait_object_pages =
    (pty_wait_capacity + PtyWaitObjectsLayout::slots_per_page - 1) / PtyWaitObjectsLayout::slots_per_page;
using PtyWaitObjects = memory::ObjectPool<PtyWait, NativePagePool, pty_wait_object_pages>;
constinit PtyWaitObjects pty_wait_objects{page_pool, pty_wait_capacity};
std::array<PollWaitIndex, pty_wait_capacity> pty_waits{};
