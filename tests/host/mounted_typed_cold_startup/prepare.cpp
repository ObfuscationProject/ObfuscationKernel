#include "fixture.hpp"
#include <filesystem>
#include <string>

using namespace typed_cold_startup_fixture;
int main(int argc, char **argv)
{
    require(argc == 3, "usage: cold-startup-native-fixture-prepare input.img new-output-directory");
    const auto input = load(argv[1]);
    const std::filesystem::path output(argv[2]);
    require(std::filesystem::create_directory(output), "exclusively create native fixture output directory");
    for (bool allocated : {false, true})
    {
        const auto prepared = prepare(input, allocated);
        const std::string family = allocated ? "allocated" : "zero";
        for (u8 mask = 0; mask < 16; ++mask)
        {
            const auto image = prepared.cut(mask);
            const auto name = output / (family + "-mask-" + std::to_string(mask) + ".img");
            save(name.c_str(), image);
        }
        const auto manifest = output / (family + "-native-history.txt");
        std::FILE *stream = std::fopen(manifest.c_str(), "wx"); require(stream != nullptr, "create native history output");
        std::fprintf(stream,
            "family=%s\nsession_serial=%llu\ninitial_sequence=%u\nactive_sequence=%u\nfinal_sequence=%u\n"
            "reads=%zu\nwrites=%zu\nflushes=%zu\nhomes=%u,%u,%u,%u\n"
            "native_session_history_modified=false\nsource_model_only=true\n",
            family.c_str(), static_cast<unsigned long long>(prepared.initial.session_serial),
            prepared.binding.journal_sequence, prepared.active_sequence, prepared.final_sequence,
            prepared.terminal.reads, prepared.terminal.writes, prepared.terminal.flushes,
            prepared.targets[0], prepared.targets[1], prepared.targets[2], prepared.targets[3]);
        require(std::fclose(stream) == 0, "close native history output");
        std::printf("NATIVE_TYPED_COLD_FIXTURE family=%s groups=4 blocks=32768 masks=16 serial=%llu sequence=%u/%u/%u reads=%zu writes=%zu flushes=%zu\n",
            family.c_str(), static_cast<unsigned long long>(prepared.initial.session_serial),
            prepared.binding.journal_sequence, prepared.active_sequence, prepared.final_sequence,
            prepared.terminal.reads, prepared.terminal.writes, prepared.terminal.flushes);
    }
    return 0;
}
