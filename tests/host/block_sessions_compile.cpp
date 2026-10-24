#include <ok/block/sessions.hpp>
using Service = ok::block::BlockSessions<2>;
ok::Status qualify_block_sessions(Service &service, ok::driver::BlockDevice &device,
                                 Service::Session &session, std::span<std::byte> bytes)
{
    const auto registration = service.register_device(device, 1);
    if (!registration) return registration.status();
    if (const auto status = service.acquire(registration.value(), {0, device.geometry().block_count},
                                           ok::block::SessionAccess::readonly, session); !status) return status;
    if (const auto status = session.read_blocks(0, bytes); !status) return status;
    return session.close();
}
