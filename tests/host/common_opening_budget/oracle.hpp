#pragma once

// ABI64 rows use the separate Python/source calculation. The appended regular
// move row uses 5 locator + 143 completion + 1 head + 11,762,118,846 continuation
// reads, as derived in docs/storage/common-opening-budget.md.
inline constexpr std::array<ok::fs::Ext4MountedCommonCapacity, 9> common_opening_oracle{{
    {58811342498ULL, 8, 6, 2, 4},
    {23524424765ULL, 7, 5, 2, 5},
    {11762325977ULL, 120, 64, 24, 4},
    {11762191265ULL, 21, 11, 5, 5},
    {23524315345ULL, 111, 61, 23, 5},
    {2917720020ULL, 5, 3, 1, 5},
    {2917720035ULL, 5, 3, 1, 5},
    {23524132523ULL, 9, 5, 2, 5},
    {5ULL + 143 + 1 + 11762118846ULL, 6, 3, 2, 5},
}};
