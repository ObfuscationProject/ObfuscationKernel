#include "../kernel/points/directory_checks.hpp"
#include <cstdio>
#include <cstdlib>

int main()
{
    using ok::abi::linux_user::Profile;
    const bool passed[] = {
        bool(ok::tests::check_directory_codec<Profile::i386>()),
        bool(ok::tests::check_directory_codec<Profile::x86_64>()),
        bool(ok::tests::check_directory_codec<Profile::aarch64>()),
        bool(ok::tests::check_directory_codec<Profile::arm32>()),
        bool(ok::tests::check_directory_codec<Profile::rv64>()),
        bool(ok::tests::check_directory_codec<Profile::rv32>()),
        bool(ok::tests::check_directory_codec<Profile::loongarch64>()),
        bool(ok::tests::check_directory_codec<Profile::mips>()),
        bool(ok::tests::check_directory_codec<Profile::mips64>()),
        bool(ok::tests::check_directory_codec<Profile::ppc>())};
    for (bool value : passed) if (!value) std::abort();
    std::puts("PASS hosted directory debug point: ten ABI codecs and free/slack relocation; scope=model");
}
