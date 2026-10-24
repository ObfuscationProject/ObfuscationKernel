#include <ok/fs/ext4_mounted_common_opening_budget.hpp>
#include <ok/fs/ext4_mounted_typed_budget.hpp>

using Budget = ok::fs::Ext4MountedCommonOpeningBudget;
static_assert(Budget::whole_union().reads == 58'811'342'498ULL);
static_assert(Budget::whole_union().writes == 120);
static_assert(Budget::whole_union().flushes == 64);
static_assert(sizeof(Budget::Counter) == 8);
static_assert(!ok::block::SessionCounterPolicy<bool>);
static_assert(!ok::block::SessionCounterPolicy<const bool>);
static_assert(!ok::block::SessionCounterPolicy<volatile bool>);
static_assert(!ok::block::SessionCounterPolicy<const volatile bool>);
static_assert(ok::block::SessionCounterPolicy<const ok::u32>);
static_assert(ok::block::SessionCounterPolicy<volatile ok::u64>);

template <typename Policy>
concept TypedBudgetPolicy = requires(const ok::block::SessionReport &initial,
                                    const ok::fs::Ext4MountedTypedCapacity &capacity)
{
    ok::fs::Ext4MountedTypedBudget::counters<Policy>(initial, capacity);
    ok::fs::Ext4MountedTypedBudget::capacity<Policy>(initial, 4, 32768,
        ok::fs::Ext4MountedTypedOrigin::startup, ok::fs::Ext4MountedTypedRoute::zero_recovery);
};
static_assert(!TypedBudgetPolicy<bool> && !TypedBudgetPolicy<const bool>);
static_assert(!TypedBudgetPolicy<volatile bool> && !TypedBudgetPolicy<const volatile bool>);
static_assert(TypedBudgetPolicy<ok::u32> && TypedBudgetPolicy<const ok::u64>);

ok::Result<Budget::Capacity> common_opening_budget_compile(const Budget::Counts &baseline,
    const Budget::Counts &endpoint, Budget::Family family, ok::u8 reads)
{
    return Budget::narrow(baseline, endpoint, family, reads);
}
