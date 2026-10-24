#include "ext4_realtime_model.hpp"

namespace
{
ok::time::RealtimeSample sample{};
ok::Status failure = ok::Status::not_initialized("hosted ext4 clock unavailable");
ok::usize invocation_count{};
}

namespace ok::testing::ext4_clock
{
void reset()
{
    sample = {};
    failure = Status::not_initialized("hosted ext4 clock unavailable");
    invocation_count = 0;
}
void set(time::RealtimeSample value)
{
    sample = value;
    failure = Status::success();
}
void fail(Status status)
{
    failure = status ? Status::io_error("hosted ext4 clock failure") : status;
}
usize calls() { return invocation_count; }
} // namespace ok::testing::ext4_clock

namespace ok::arch
{
Result<time::RealtimeSample> realtime_sample()
{
    ++invocation_count;
    return failure ? Result<time::RealtimeSample>(sample)
                   : Result<time::RealtimeSample>(failure);
}
} // namespace ok::arch
