// Included inside the serialized runtime's private namespace.
using LinuxUdpReceiveWait = LinuxUdpReceiveOwner<lu::configured_native_profile()>;
using LinuxUdpReceiveAdmissionToken = LinuxUdpReceiveAdmission<lu::configured_native_profile()>;
using LinuxUdpReceiveTransferCoordinator = LinuxUdpReceiveCoordinator<lu::configured_native_profile()>;
static_assert(!std::is_copy_constructible_v<LinuxUdpReceiveWait>);
static_assert(std::is_nothrow_move_constructible_v<LinuxUdpReceiveWait>);
static_assert(std::is_nothrow_destructible_v<LinuxUdpReceiveWait>);
constexpr usize linux_udp_receive_capacity = memory::KernelEntryStackPool::capacity;
using LinuxUdpReceiveStore = LinuxUdpReceiveOwnerStore<lu::configured_native_profile(), NativePagePool>;
using LinuxUdpReceiveLayout = LinuxUdpReceiveStore::Layout;
constexpr usize linux_udp_receive_pages = LinuxUdpReceiveStore::pages;
using LinuxUdpReceiveObjects = LinuxUdpReceiveStore::Objects;
constinit LinuxUdpReceiveStore linux_udp_receive_store{syscall_origins,page_pool};
constinit auto &linux_udp_receive_objects=linux_udp_receive_store.objects();
constinit auto &linux_udp_receive_waits=linux_udp_receive_store.members();
