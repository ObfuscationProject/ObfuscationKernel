#pragma once
#include "ok/core/types.hpp"

namespace ok { class Kernel; }
namespace ok::user::native
{
// Called only from the serialized service context, with no active user entry.
// A successful completion stays waitable or awaits the checked service reaper.
// Administrative callers may reap a native-personality completion afterward.
[[nodiscard]] Status terminate_native_process(Kernel &kernel, u64 pid, u8 signal);
}
