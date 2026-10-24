// Included inside the serialized runtime's private namespace.
enum class NativeUdpOwnerPhase : u8 { staged, published, taken };
struct NativeUdpReceiveWait
{
    sched::ProcessId pid{};
    sched::ThreadId tid{};
    NativeUdpOwnerPhase phase{NativeUdpOwnerPhase::staged};
    SyscallOrigins::NativeUdpCapture capture;
    SyscallOrigins::NativeUdpReceiveRequest operation{};
    u32 endpoint{};
    u16 retained_description{sched::invalid_description};
    sched::WaitTicket ticket{};
    u64 request{}, observed_receive{}, observed_executor{};
    std::optional<SyscallOrigins::NativeUdpReceiveBinding> origin_binding;

    NativeUdpReceiveWait(sched::ProcessId owner, sched::ThreadId thread,
        SyscallOrigins::NativeUdpCapture captured,
        SyscallOrigins::NativeUdpReceiveRequest copied, u32 handle) noexcept
        : pid(owner), tid(thread), capture(captured), operation(copied), endpoint(handle) {}
};
static_assert(std::is_nothrow_copy_constructible_v<NativeUdpReceiveWait>);
static_assert(std::is_nothrow_destructible_v<NativeUdpReceiveWait>);
constexpr usize native_udp_receive_capacity = memory::KernelEntryStackPool::capacity;
using NativeUdpReceiveLayout = memory::ObjectPool<NativeUdpReceiveWait, NativePagePool, 1>;
constexpr usize native_udp_receive_pages =
    (native_udp_receive_capacity + NativeUdpReceiveLayout::slots_per_page - 1) /
    NativeUdpReceiveLayout::slots_per_page;
using NativeUdpReceiveObjects = memory::ObjectPool<NativeUdpReceiveWait, NativePagePool, native_udp_receive_pages>;
constinit NativeUdpReceiveObjects native_udp_receive_objects{page_pool, native_udp_receive_capacity};
std::array<PollWaitIndex, native_udp_receive_capacity> native_udp_receive_waits{};
