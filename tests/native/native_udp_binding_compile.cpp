#include "ok/net/native_udp.hpp"

extern "C" ok::u32 native_udp_binding_probe(const ok::net::NativeUdpService *service, ok::u32 handle)
{
    const auto snapshot = service->binding(handle);
    if (!snapshot) return static_cast<ok::u32>(snapshot.status().code());
    return snapshot.value().bound ? snapshot.value().port : 0;
}
