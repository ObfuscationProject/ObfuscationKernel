// Actual backend slot bookkeeping with mocked initialization; no native entry or isolation claim.
#if defined(OK_TEST_EXECUTION_STUB)
#include "../../src/arch/execution_stub.cpp"
#else
#include "../../src/arch/x86_64/execution.cpp"
#endif

#include <iostream>

int main()
{
    using namespace ok;
    using namespace ok::arch::execution;
    auto report = ownership_report();
#if defined(OK_TEST_EXECUTION_STUB)
    if (report.failure.code() != StatusCode::unsupported || report.address_spaces || report.executing) return 1;
    std::cout << "EXECUTION_OWNERSHIP_HOST_PASS backend=unsupported-stub scope=hosted-readonly\n";
#else
    if (!report.failure || report.address_spaces || report.executing) return 1;
    initialized = true;
    PageMapping mapping{user_begin, 0x100000, page_read};
    auto created = create_space({&mapping, 1});
    if (!created) return 2;
    auto handle = created.value();
    auto stale = handle;
    report = ownership_report();
    if (!report.failure || report.address_spaces != 1 || report.executing) return 3;
    // No scheduler or process record was published for this actual backend slot.
    const auto generation = spaces[handle.slot].generation;
    auto again = ownership_report();
    if (again.address_spaces != 1 || spaces[handle.slot].generation != generation) return 4;
    executing = true;
    if (!ownership_report().executing || destroy_space(handle).code() != StatusCode::busy) return 5;
    executing = false;
    UserFrame frame{};
    ok_x86_execution_active_frame = &frame;
    if (!ownership_report().executing) return 6;
    ok_x86_execution_active_frame = nullptr;
    ok_x86_execution_resume_rsp = 1;
    if (!ownership_report().executing) return 7;
    ok_x86_execution_resume_rsp = 0;
    ok_x86_execution_user_rsp = 1;
    if (!ownership_report().executing) return 8;
    ok_x86_execution_user_rsp = 0;
    spaces[handle.slot].generation = 0;
    if (ownership_report().failure.code() != StatusCode::io_error) return 9;
    spaces[handle.slot].generation = generation;
    const auto tables = spaces[handle.slot].tables_used;
    spaces[handle.slot].tables_used = table_count + 1;
    if (ownership_report().failure.code() != StatusCode::io_error) return 10;
    spaces[handle.slot].tables_used = tables;
    initialized = false;
    if (ownership_report().failure.code() != StatusCode::io_error) return 11;
    initialized = true;
    if (!destroy_space(handle) || handle.root) return 12;
    report = ownership_report();
    if (!report.failure || report.address_spaces || report.executing) return 13;
    // Static reusable table bytes survive destruction but are not live owners.
    if (spaces[stale.slot].tables_used != tables ||
        destroy_space(stale).code() != StatusCode::not_found || !ownership_report().failure) return 14;
    std::cout << "EXECUTION_OWNERSHIP_HOST_PASS backend=actual-x86_64-slot-bookkeeping cases=14 scope=hosted-no-native-entry\n";
#endif
}
