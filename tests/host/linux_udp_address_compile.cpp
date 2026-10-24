#include "ok/abi/linux_udp_address.hpp"

// Normal-access freestanding compilation instantiates every preserved codec.
namespace ok::abi::linux_user
{
template class Ipv4SocketAddressCodec<Profile::i386>;
template class Ipv4SocketAddressCodec<Profile::x86_64>;
template class Ipv4SocketAddressCodec<Profile::aarch64>;
template class Ipv4SocketAddressCodec<Profile::arm32>;
template class Ipv4SocketAddressCodec<Profile::rv64>;
template class Ipv4SocketAddressCodec<Profile::rv32>;
template class Ipv4SocketAddressCodec<Profile::loongarch64>;
template class Ipv4SocketAddressCodec<Profile::mips>;
template class Ipv4SocketAddressCodec<Profile::mips64>;
template class Ipv4SocketAddressCodec<Profile::ppc>;
}
