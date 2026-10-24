#include "ok/abi/linux_udp_call.hpp"

namespace ok::abi::linux_user
{
struct ProbeCopy final
{
    Status operator()(u64, std::span<u8> bytes)
    {
        for (auto &byte : bytes) byte = 0;
        return Status::success();
    }
};
template <Profile Selected> Result<UdpRequest> probe(const Call &raw, const RegisterFile &frame,
                                                    std::span<const u8> stack, ProbeCopy &copy,
                                                    std::span<u8> output)
{
    static_cast<void>(UdpCallDecoder<Selected>::decode_direct(raw));
    static_cast<void>(UdpCallDecoder<Selected>::decode_direct(frame, stack));
    static_cast<void>(UdpCallDecoder<Selected>::decode_socketcall(frame, copy));
    static_cast<void>(UdpWire<Selected>::decode_address(stack));
    static_cast<void>(UdpWire<Selected>::encode_address(output, {}));
    static_cast<void>(UdpWire<Selected>::decode_socklen(stack));
    static_cast<void>(UdpWire<Selected>::encode_socklen(output, 16));
    static_cast<void>(UdpConstants<Selected>::decode_error_number(StatusCode::fault));
    return UdpCallDecoder<Selected>::decode_socketcall(raw, copy);
}
#define OK_UDP_PROBE(profile) template Result<UdpRequest> probe<Profile::profile>(const Call &, const RegisterFile &, std::span<const u8>, ProbeCopy &, std::span<u8>);
OK_UDP_PROBE(i386)
OK_UDP_PROBE(x86_64)
OK_UDP_PROBE(aarch64)
OK_UDP_PROBE(arm32)
OK_UDP_PROBE(rv64)
OK_UDP_PROBE(rv32)
OK_UDP_PROBE(loongarch64)
OK_UDP_PROBE(mips)
OK_UDP_PROBE(mips64)
OK_UDP_PROBE(ppc)
#undef OK_UDP_PROBE
}
