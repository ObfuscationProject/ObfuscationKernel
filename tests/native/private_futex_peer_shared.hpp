#pragma once
namespace ok::tests
{
using PeerWord = __UINT32_TYPE__;
inline constexpr PeerWord peer_magic = 0x46555458;
inline constexpr PeerWord peer_unwritten = 0xfedcba98;
struct PeerState
{
    PeerWord magic, version, word, park;
    PeerWord entered[4], completed[4], failure[4];
    PeerWord results[2][3], observed[2][3], wakes[2][3];
};
static_assert(sizeof(PeerState) == 136);
}
