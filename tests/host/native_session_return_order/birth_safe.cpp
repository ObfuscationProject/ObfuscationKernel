// Reuse ordinary typed lifecycles without running retained field mutations.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main retained_birth_main
#include "../completed_ownership_birth/model.cpp"
#undef main
#pragma GCC diagnostic pop

void detached_progress_table()
{
    // These standalone values have no Session, child, Reader or callback. The
    // truth table independently checks the stage/flag invariant as a pure value
    // predicate; it never changes a live kernel or typed-child record.
    using Stage = BirthProgress::Stage;
    unsigned identity{};
    for (Stage stage : {Stage::unused, Stage::proving, Stage::global, Stage::target})
        for (bool global : {false, true})
            for (bool target : {false, true})
            {
                Birth seal;
                BirthProgress progress;
                progress.stage_ = stage;
                if (stage != Stage::unused) { progress.child_ = &identity; progress.seal_ = &seal; }
                seal.global_minted_ = global; seal.target_minted_ = target;
                const bool expected = stage == Stage::unused || stage == Stage::proving ? !global && !target :
                    stage == Stage::global ? global && !target : global && target;
                CHECK(Validator::completed_birth_progress_exact(seal, progress, &identity) == expected);
                CHECK(Validator::completed_birth_uncaptured(seal, progress, &identity) ==
                    (expected && (stage == Stage::unused || stage == Stage::proving)));
                if (stage != Stage::unused)
                    CHECK(!Validator::completed_birth_progress_exact(seal, progress, &seal));
            }
}
int main()
{
    detached_progress_table();
    for (bool cross : {false, true})
    {
        run(false, false, cross, 0, 0, false);
        run(false, false, cross, 3, 0, false);
        run(true, false, cross, 0, 0, false);
        for (u8 mask : {u8{0}, u8{5}, u8{15}})
        {
            run(false, true, cross, 3, mask, false);
            run(true, true, cross, 0, mask, false);
        }
    }
    std::printf("COMPLETED_OWNERSHIP_BIRTH_SAFE fixtures=%zu checks=%zu comparisons=%zu global_callbacks=%zu target_callbacks=%zu detached_stage_cases=16\n",
        fixtures, checks, comparisons, global_callbacks, target_callbacks);
    return 0;
}
