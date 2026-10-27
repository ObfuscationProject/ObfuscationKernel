#include "ok/arch/x86_64/signal.hpp"
using Codec = ok::user::native::X86SignalFrame;
extern bool signal_frame_read(ok::u64, std::span<std::byte>);
bool signal_frame_compile(ok::u64 sp, const Codec::Frame &current, Codec::Restored &restored)
{
    return Codec::decode(sp, current, 0x40000000, 0x80000000, 0xffbf, signal_frame_read, restored);
}
Codec::Image signal_frame_encode_compile(const Codec::Frame &frame, Codec::Placement placement)
{
    return Codec::encode(frame, {}, {}, {10, -6, 1, 1}, placement, 0x40001000, 0xffbf);
}
