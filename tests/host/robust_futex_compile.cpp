#include "ok/user/robust_futex.hpp"
using namespace ok;
using namespace ok::user::native;
constexpr auto selected=abi::linux_user::Profile::ROBUST_PROFILE;
static_assert(sizeof(uptr)==RobustListCodec<selected>::word_bytes);
static_assert((std::endian::native==std::endian::big)==RobustListCodec<selected>::big_endian);
struct Access
{
    static constexpr bool serialized_native_execution=true;
    Status read(u64,std::span<std::byte>);
    Result<u32> compare_exchange32(u64,u32,u32);
    void wake_one(u64);
};
extern "C" RobustWalkReport robust_compile_only(u64 head,u32 tid,Access &access)
{
    return retire_robust_list<selected>(head,tid,access);
}
