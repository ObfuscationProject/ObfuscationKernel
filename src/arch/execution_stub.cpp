#include "ok/arch/execution.hpp"

namespace ok::arch::execution
{
bool supported() noexcept
{
    return false;
}
Status initialize()
{
    return Status::unsupported("native user execution is not implemented for this architecture");
}
Result<AddressSpace> create_space(std::span<const PageMapping>)
{
    return initialize();
}
Status destroy_space(AddressSpace &)
{
    return initialize();
}
Result<Trap> enter(AddressSpace &, UserFrame &)
{
    return initialize();
}
UserFrame initial_frame(uptr entry, uptr stack)
{
    UserFrame frame{};
    frame.pc = entry;
    frame.sp = stack;
    return frame;
}
Syscall syscall(const UserFrame &)
{
    return {};
}
void set_return_value(UserFrame &, i64)
{
}
bool user_range(const AddressSpace &, uptr, usize, bool)
{
    return false;
}
Status copy_from_user(const AddressSpace &, void *, uptr, usize)
{
    return initialize();
}
Status copy_to_user(const AddressSpace &, uptr, const void *, usize)
{
    return initialize();
}
} // namespace ok::arch::execution
