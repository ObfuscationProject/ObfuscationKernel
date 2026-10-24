#include "ok/user/native_signal_mask.hpp"
using namespace ok;
using namespace ok::user::native;
constexpr auto selected = abi::linux_user::Profile::MASK_PROFILE;
static_assert(sizeof(uptr) == SignalMaskCodec<selected>::word_bytes);
static_assert((std::endian::native == std::endian::big) == abi::linux_user::Codec<selected>::profile.big_endian);
struct Read { Status operator()(u64, std::span<std::byte>); };
struct Write { Status operator()(u64, std::span<const std::byte>); };
extern "C" i64 signal_mask_compile_only(SignalMask &mask, i32 how, u64 input, u64 output, u64 size)
{
    return update_signal_mask<selected>(mask, how, input, output, size, Read{}, Write{});
}
