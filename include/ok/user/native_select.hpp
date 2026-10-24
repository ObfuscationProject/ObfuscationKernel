#pragma once
#include "ok/user/native_poll.hpp"
#include "ok/sched/resources.hpp"

namespace ok::user::native
{
// Target wire shape is copied on admission. One bit per local descriptor;
// duplicated membership in different sets does not duplicate its lifetime pin.
struct SelectSets
{
    std::array<uptr, 3> output{};
    std::array<u32, 3> interests{};
    usize count{};
    u8 word_bytes{};
};
struct SelectRequest
{
    SelectSets sets{};
    // Carries deadline and timeout output only. Admission supplies the dense
    // descriptors, Linux provider policy and set-output representation.
    PollRequest readiness{};
};
i64 begin_select(Kernel &kernel, sched::ProcessId pid, const SelectRequest &request, bool &complete);
} // namespace ok::user::native
