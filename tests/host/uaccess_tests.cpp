// The uaccess contract (ok/user/uaccess.hpp) against a mock backend and a
// mock resolution hook: a refused access is resolved and retried at most
// max_retries times, then attempted once more holding mm; a space no process
// owns keeps its first result; with_user_word never resolves and never takes
// a lock; user copies under a class A lock count class_asserts.
// Model coverage only (scope=model).

#include "ok/user/uaccess.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ok;
namespace hw = ok::arch::execution;
namespace uaccess = ok::user::uaccess;

#define CHECK(condition)                                                                                   \
    do                                                                                                     \
    {                                                                                                      \
        if (!static_cast<bool>(condition))                                                                 \
        {                                                                                                  \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                            \
            std::abort();                                                                                  \
        }                                                                                                  \
    } while (false)

namespace mock
{
alignas(4096) std::array<u8, 4096> page{};
// The backend refuses writes while `withheld`, and refuses everything while
// `unmapped`. Each resolution clears one withheld step.
usize withheld{};
bool unmapped{};
bool owned{true};
usize attempts{}, resolutions{}, held_attempts{};
bool holding{};
constexpr uptr base = hw::user_begin;
bool reachable(uptr address, usize size, bool write)
{
    return !unmapped && address >= base && size <= page.size() && address - base <= page.size() - size &&
           !(write && withheld);
}
} // namespace mock

namespace ok::arch::execution
{
bool user_range(const AddressSpace &, uptr address, usize size, bool write)
{
    return mock::reachable(address, size, write);
}
Status copy_from_user(const AddressSpace &, void *destination, uptr source, usize size)
{
    ++mock::attempts;
    mock::held_attempts += mock::holding;
    if (!mock::reachable(source, size, false)) return Status::fault();
    std::memcpy(destination, mock::page.data() + (source - mock::base), size);
    return {};
}
Status copy_to_user(const AddressSpace &, uptr destination, const void *source, usize size)
{
    ++mock::attempts;
    mock::held_attempts += mock::holding;
    if (!mock::reachable(destination, size, true)) return Status::fault();
    std::memcpy(mock::page.data() + (destination - mock::base), source, size);
    return {};
}
Result<u32 *> user_word(const AddressSpace &, uptr address, bool write)
{
    if (address % sizeof(u32)) return Status::invalid_argument();
    if (!mock::reachable(address, sizeof(u32), write)) return Status::fault();
    return reinterpret_cast<u32 *>(mock::page.data() + (address - mock::base));
}
} // namespace ok::arch::execution

// The runtime's hook, modelled: resolves one withheld step per call.
ok::Status ok::user::native::resolve_user_access(const arch::execution::AddressSpace &, uptr, usize, bool write,
                                                 const uaccess::Attempt *attempt)
{
    if (!mock::owned) return attempt ? (*attempt)() : Status::no_process();
    mock::holding = true;
    usize resolved = 0;
    if (write && mock::withheld)
    {
        --mock::withheld;
        ++mock::resolutions;
        resolved = 1;
    }
    Status result = attempt ? (*attempt)() : resolved ? Status{} : Status::not_found();
    mock::holding = false;
    return result;
}

namespace
{
void reset()
{
    mock::withheld = 0;
    mock::unmapped = false;
    mock::owned = true;
    mock::attempts = mock::resolutions = mock::held_attempts = 0;
}

void check_retries()
{
    const hw::AddressSpace space{1, 0, 1};
    const u32 value = 0x1234;
    reset();
    CHECK(uaccess::copy_to_user(space, mock::base, &value, sizeof(value)));
    CHECK(mock::attempts == 1 && mock::resolutions == 0);

    // One withheld page: resolved, then the retry succeeds.
    reset();
    mock::withheld = 1;
    CHECK(uaccess::copy_to_user(space, mock::base + 8, &value, sizeof(value)));
    CHECK(mock::attempts == 2 && mock::resolutions == 1 && mock::held_attempts == 0);

    // Withheld again after every resolution (a sibling keeps re-protecting):
    // max_retries rounds, then one final attempt holding mm.
    reset();
    mock::withheld = uaccess::max_retries + 1;
    CHECK(uaccess::copy_to_user(space, mock::base + 8, &value, sizeof(value)));
    CHECK(mock::attempts == uaccess::max_retries + 2 && mock::held_attempts == 1);
    CHECK(mock::resolutions == uaccess::max_retries + 1);

    // A genuine fault: nothing to resolve, one attempt holding mm, EFAULT.
    reset();
    mock::unmapped = true;
    CHECK(uaccess::copy_to_user(space, mock::base, &value, sizeof(value)).code() == StatusCode::fault);
    CHECK(mock::attempts == 2 && mock::held_attempts == 1 && mock::resolutions == 0);
    u32 out{};
    CHECK(uaccess::copy_from_user(space, &out, mock::base, sizeof(out)).code() == StatusCode::fault);
    CHECK(!uaccess::user_range(space, mock::base, 4, true));

    // No live owner: the first result stands, no mm.
    reset();
    mock::owned = false;
    mock::withheld = 1;
    CHECK(uaccess::copy_to_user(space, mock::base, &value, sizeof(value)).code() == StatusCode::fault);
    CHECK(mock::attempts == 1 && mock::held_attempts == 0);

    // user_range resolves for a write as a copy does.
    reset();
    mock::withheld = 1;
    CHECK(uaccess::user_range(space, mock::base, 64, true) && mock::resolutions == 1);
    CHECK(uaccess::user_range(space, mock::base, 64, false));
}

void check_user_word()
{
    const hw::AddressSpace space{1, 0, 1};
    reset();
    std::memset(mock::page.data(), 0, 16);
    u32 seen = 1;
    CHECK(uaccess::with_user_word(space, mock::base + 4, uaccess::WordAccess::write,
                                  [&](u32 &word) { seen = word; word = 7; }) == uaccess::WordResult::done);
    CHECK(seen == 0 && reinterpret_cast<u32 *>(mock::page.data())[1] == 7);
    CHECK(uaccess::with_user_word(space, mock::base + 2, uaccess::WordAccess::read, [](u32 &) {}) ==
          uaccess::WordResult::misaligned);
    // A withheld word is not resolved here: the caller drops its locks first.
    mock::withheld = 1;
    CHECK(uaccess::with_user_word(space, mock::base + 4, uaccess::WordAccess::write, [](u32 &) {}) ==
          uaccess::WordResult::needs_resolve);
    CHECK(mock::resolutions == 0);
    CHECK(uaccess::with_user_word(space, mock::base + 4, uaccess::WordAccess::read,
                                  [&](u32 &word) { seen = word; }) == uaccess::WordResult::done && seen == 7);
    // The word access runs inside a kernel window and closes it.
    CHECK(!smp::in_kernel_window());
    // Under a class A lock it is the one allowed user access; a copy is not.
    smp::RankedSpinLock<smp::LockRank::task> lock;
    const auto before = smp::check_total(smp::KernelCheck::class_rule);
    lock.lock();
    CHECK(uaccess::with_user_word(space, mock::base + 4, uaccess::WordAccess::read, [](u32 &) {}) ==
          uaccess::WordResult::done);
    CHECK(smp::check_total(smp::KernelCheck::class_rule) == before);
    u32 out{};
    CHECK(uaccess::copy_from_user(space, &out, mock::base, sizeof(out)));
    lock.unlock();
    CHECK(smp::check_total(smp::KernelCheck::class_rule) == before + (smp::lock_debug_enabled ? 1 : 0));
}
} // namespace

int main()
{
    check_retries();
    check_user_word();
    std::printf("OK_UACCESS_MODEL scope=model retries=pass final_attempt=pass ownerless=pass user_word=pass "
                "class_rule=pass debug=%d\n",
                smp::lock_debug_enabled ? 1 : 0);
    return 0;
}
