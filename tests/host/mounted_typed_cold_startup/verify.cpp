#include "fixture.hpp"

using namespace typed_cold_startup_fixture;
int main(int argc, char **argv)
{
    require(argc == 2, "usage: cold-startup-native-fixture-verify input.img");
    const auto input = load(argv[1]);
    for (bool allocated : {false, true})
    {
        const auto prepared = prepare(input, allocated);
        for (u8 mask = 0; mask < 16; ++mask)
        {
            const auto terminal = verify_native_recovery(prepared, mask);
            std::printf("NATIVE_TYPED_CUT_VERIFY allocated=%u mask=%u writer_crash_reachable=%u serial=%llu reads=%zu writes=%zu flushes=%zu all_bytes_equal=yes\n",
                unsigned(allocated), unsigned(mask), unsigned(prepared.writer_crash_reachable(mask)), static_cast<unsigned long long>(terminal.session_serial),
                terminal.reads, terminal.writes, terminal.flushes);
        }
    }
    std::puts("NATIVE_TYPED_CUT_VERIFY_COMPLETE families=2 cuts=32 source_model_only=yes");
    return 0;
}
