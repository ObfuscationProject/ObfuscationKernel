#include "ok/user/native_signal_trampoline.hpp"

using Profile = ok::abi::linux_user::Profile;
using Selected = ok::user::native::SignalTrampoline<Profile::OK_TRAMPOLINE_PROFILE>;
static_assert(sizeof(Selected::image()) == Selected::code_bytes);

extern "C"
{
[[gnu::used, gnu::section(".rodata.signal_return")]]
extern const auto ok_signal_trampoline_blob = Selected::image();

bool ok_signal_trampoline_address(ok::u64 base, ok::u8 entry, ok::u64 begin,
                                 ok::u64 end, ok::u64 *out)
{
    return Selected::entry_address(base, static_cast<ok::user::native::SignalTrampolineEntry>(entry),
                                   begin, end, *out);
}
}
