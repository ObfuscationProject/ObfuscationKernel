#include "ok/fs/mode_policy.hpp"

#include <cstdio>

namespace
{
ok::usize checks{};
bool check(bool value)
{
    ++checks;
    if (!value)
        std::fprintf(stderr, "mode policy check %zu failed\n", checks);
    return value;
}
}

int main()
{
    using ok::fs::mode_change_policy;
    using ok::user::Credentials;
    // Exhaust every stored permission combination, independently checking each
    // bit and ensuring real IDs or unrelated request bits cannot grant authority.
    for (ok::u32 bits = 0; bits <= 07777u; ++bits)
    {
        for (ok::u32 high : {0u, 0100000u, 0xfffff000u})
        {
            const auto owner = mode_change_policy(1000, 2000, bits | high,
                Credentials{.uid = 71, .gid = 72, .euid = 1000, .egid = 2000});
            const auto root = mode_change_policy(1000, 2000, bits | high,
                Credentials{.uid = 71, .gid = 72, .euid = 0, .egid = 73});
            const auto foreign_group = mode_change_policy(1000, 2000, bits | high,
                Credentials{.uid = 71, .gid = 2000, .euid = 1000, .egid = 73});
            const auto nonowner = mode_change_policy(1000, 2000, bits | high,
                Credentials{.uid = 1000, .gid = 2000, .euid = 71, .egid = 2000});
            if (!check(owner.authorized && root.authorized && foreign_group.authorized && !nonowner.authorized))
                return 1;
            if (!check(owner.permissions < 010000u && root.permissions < 010000u && foreign_group.permissions < 010000u))
                return 2;
            for (ok::u32 bit = 1; bit <= 04000u; bit <<= 1)
            {
                if (!check((owner.permissions & bit) == (bits & bit) && (root.permissions & bit) == (bits & bit)))
                    return 3;
                const bool expected = bit != 02000u && (bits & bit) != 0;
                if (!check(((foreign_group.permissions & bit) != 0) == expected))
                    return 4;
            }
        }
    }
    if (!check(!mode_change_policy(1000, 2000, 07777,
            Credentials{.uid = 0, .gid = 0, .euid = 71, .egid = 2000, .kernel_space = true}).authorized))
        return 5;
    std::printf("PASS MODE_POLICY_MODEL checks=%zu effective-ids=pass owner=pass sgid=pass guest=0\n", checks);
}
