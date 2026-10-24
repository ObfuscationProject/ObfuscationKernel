// Target compilation probe only; no user or device execution is implied.
#include "ok/abi/linux_chmod_call.hpp"

extern "C" ok::i64 chmod_target_compile_probe(ok::abi::linux_user::ChmodCall call,
                                                const ok::u64 *words, ok::usize count)
{
    using namespace ok;
    using namespace abi::linux_user;
    auto read = [](u64, FixedString<256> &) { return Status::fault("compile-only user copier"); };
    auto change = [](ChmodRequest, std::string_view) { return fs::ModeChangeResult::unavailable(); };
    return ChmodCallAdapter<Profile::MODE_TEST_PROFILE>::execute(call, {words, count}, read, change);
}
