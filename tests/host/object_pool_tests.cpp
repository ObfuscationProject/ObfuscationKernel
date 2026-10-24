#include "ok/memory/object_pool.hpp"

#include <cstdio>
#include <cstdlib>

namespace ok::memory
{
struct ObjectPoolTestAccess
{
    template <class Pool> static void near_exhaustion(Pool &pool) { pool.generation_ = ~u64{0} - 1; }
};
} // namespace ok::memory

namespace
{
using namespace ok;
using namespace ok::memory;
usize checks{};
void check(bool value, const char *message)
{
    ++checks;
    if (!value)
    {
        std::fprintf(stderr, "FAIL object-pool checks=%zu: %s\n", checks, message);
        std::exit(1);
    }
}

struct Provider
{
    static constexpr usize page_size = 4096;
    static constexpr usize pages = 8;
    struct Lease
    {
        std::span<std::byte> bytes{};
        const Provider *owner{};
        usize index{};
        u64 generation{};
    };
    alignas(page_size) std::array<std::array<std::byte, page_size>, pages> backing{};
    std::array<u64, pages> generations{};
    u64 sequence{};
    usize allocations{}, releases{}, failures{}, attempts{};
    usize available{pages};
    bool fail_allocate{}, fail_release{};
    enum class Invalid { none, short_page, null_page, unaligned, wrap, alias } invalid{};
    Lease first{};

    Result<Lease> allocate_kernel_page()
    {
        ++attempts;
        if (fail_allocate)
            return Status::no_memory("injected backing exhaustion");
        if (invalid == Invalid::alias)
            return first;
        for (usize i = 0; i < available; ++i)
            if (generations[i] == 0)
            {
                generations[i] = ++sequence;
                ++allocations;
                backing[i].fill(std::byte{0xa7});
                Lease result{{backing[i].data(), page_size}, this, i, sequence};
                if (invalid == Invalid::short_page) result.bytes = {backing[i].data(), page_size - 1};
                if (invalid == Invalid::null_page) result.bytes = {static_cast<std::byte *>(nullptr), page_size};
                if (invalid == Invalid::unaligned) result.bytes = {backing[i].data() + 1, page_size};
                if (invalid == Invalid::wrap) result.bytes = {reinterpret_cast<std::byte *>(~uptr{0} - 7), page_size};
                if (!first.owner) first = result;
                return result;
            }
        return Status::no_memory("provider physical page limit");
    }
    Status release_kernel_page(Lease lease)
    {
        ++releases;
        if (fail_release)
        {
            ++failures;
            return Status::io_error("injected uncertain page retirement");
        }
        if (lease.owner != this || lease.index >= available || !lease.generation ||
            generations[lease.index] != lease.generation)
            return Status::invalid_argument("invalid modeled page lease");
        generations[lease.index] = 0;
        return Status::success();
    }
    usize owned() const
    {
        usize result{};
        for (const auto generation : generations) result += generation != 0;
        return result;
    }
};
static_assert(ObjectPageProvider<Provider>);

struct alignas(64) Item
{
    static inline usize constructed{}, destroyed{};
    u64 value{};
    std::array<std::byte, 56> unused{};
    explicit Item(u64 input = 0) noexcept : value(input) { ++constructed; }
    ~Item() noexcept { ++destroyed; }
};
static_assert(sizeof(Item) == 64);
using Pool = ObjectPool<Item, Provider, 2>;
static_assert(Pool::slots_per_page == 64 && Pool::max_pages == 2 && Pool::capacity == 128);
static_assert(!std::is_copy_constructible_v<Pool> && !std::is_move_constructible_v<Pool>);
Provider static_provider;
[[maybe_unused]] constinit Pool static_pool{static_provider, 64};
struct TooLarge { std::byte bytes[4097]; };
struct alignas(8192) TooAligned { std::byte byte; };
struct ThrowingDestructor { ~ThrowingDestructor() noexcept(false) {} };
struct ThrowingConstructor { ThrowingConstructor() noexcept(false) {} };
template <class T> concept AdmittedType = requires { typename ObjectPool<T, Provider, 1>; };
template <class T> concept DefaultCreatable = requires(T &pool) { pool.create(); };
static_assert(!AdmittedType<TooLarge> && !AdmittedType<TooAligned> && !AdmittedType<ThrowingDestructor>);
static_assert(!DefaultCreatable<ObjectPool<ThrowingConstructor, Provider, 1>>);

void lifecycle()
{
    Provider provider;
    Pool pool(provider);
    const usize constructors = Item::constructed, destructors = Item::destroyed;
    std::array<ObjectHandle, Pool::capacity> handles{};
    std::array<Item *, Pool::capacity> addresses{};
    for (usize i = 0; i < handles.size(); ++i)
    {
        auto created = pool.create(u64(i + 1));
        check(created.ok(), "allocate every declared object slot");
        handles[i] = created.value();
        auto object = pool.resolve(handles[i]);
        check(object.ok() && object.value()->value == i + 1, "construct target object in backing bytes");
        addresses[i] = object.value();
        check(reinterpret_cast<uptr>(object.value()) % alignof(Item) == 0, "object alignment");
        check(handles[i].generation == i + 1, "monotonic pool generation");
        for (auto byte : object.value()->unused) check(byte == std::byte{}, "object starts zero initialized");
        for (usize j = 0; j < i; ++j)
            check(pool.resolve(handles[j]).value() == addresses[j] && addresses[j]->value == j + 1,
                  "new objects never move or overwrite previous owners");
    }
    const auto full = pool.accounting();
    check(full.live_objects == 128 && full.backing_pages == 2 && full.allocations == 128 &&
          full.empty_pages == 0 && provider.owned() == 2, "full accounting");
    check(pool.create().status().code() == StatusCode::no_memory && provider.attempts == 2, "object limit before provider");
    check(pool.trim_empty_pages().ok() && provider.releases == 0, "trim leaves all live pages pinned");
    for (usize i = 0; i < handles.size(); ++i)
    {
        check(pool.destroy(handles[i]).ok(), "destroy owner exactly once");
        check(!pool.resolve(handles[i]) && !pool.destroy(handles[i]), "stale access and duplicate destroy reject");
    }
    check(Item::constructed - constructors == 128 && Item::destroyed - destructors == 128,
          "constructor/destructor count matches published owners");
    check(pool.accounting().empty_pages == 2 && pool.trim_empty_pages().ok() && !provider.owned(),
          "empty pages retire only explicitly");
    const auto reuse = pool.create(u64{9001});
    check(reuse.ok() && reuse.value().generation == 129 && pool.resolve(reuse.value()).value()->value == 9001,
          "trim does not reset object generation");
    for (const auto old : handles)
        check(!pool.resolve(old) && !pool.destroy(old), "old slab handles cannot refer to reused storage");
    const Pool &readonly = pool;
    check(readonly.resolve(reuse.value()).ok(), "const resolve retains identity");
    check(pool.destroy(reuse.value()).ok() && pool.trim_empty_pages().ok(), "reuse cleanup");
}

void refusals()
{
    Provider a, b;
    Pool first(a, 1), second(b, 1);
    auto owned = first.create(u64{123}), foreign = second.create(u64{456});
    check(owned.ok() && foreign.ok(), "foreign test setup");
    check(!first.resolve(foreign.value()) && !first.destroy(foreign.value()), "foreign pool identity refuses");
    const auto before = first.accounting();
    for (unsigned field = 0; field < 5; ++field)
    {
        auto bad = owned.value();
        if (field == 0) bad.pool = nullptr;
        if (field == 1) bad.page_slot = 2;
        if (field == 2) bad.object_slot = 64;
        if (field == 3) bad.generation = 0;
        if (field == 4) ++bad.generation;
        check(!first.resolve(bad) && !first.destroy(bad), "forged handles cannot destroy an object");
    }
    check(first.accounting().destructions == before.destructions && first.resolve(owned.value()).value()->value == 123,
          "invalid operations preserve object and accounting");
    check(first.create().status().code() == StatusCode::no_memory && a.attempts == 1, "custom object limit");
    check(first.destroy(owned.value()).ok() && first.trim_empty_pages().ok(), "first cleanup");
    check(second.destroy(foreign.value()).ok() && second.trim_empty_pages().ok(), "second cleanup");
    Pool zero(a, 0), oversized(a, Pool::capacity + 1);
    const auto attempts = a.attempts;
    check(zero.create().status().code() == StatusCode::invalid_argument &&
          oversized.create().status().code() == StatusCode::invalid_argument && a.attempts == attempts,
          "invalid live limit fails before provider");
    Pool exhausted(a);
    ObjectPoolTestAccess::near_exhaustion(exhausted);
    const auto final = exhausted.create();
    check(final.ok() && final.value().generation == ~u64{0}, "last nonzero generation is usable");
    check(exhausted.destroy(final.value()).ok() && exhausted.trim_empty_pages().ok(), "last generation cleanup");
    const auto final_attempts = a.attempts;
    check(exhausted.create().status().code() == StatusCode::overflow && a.attempts == final_attempts &&
          !exhausted.resolve(final.value()) && !exhausted.destroy(final.value()),
          "generation never wraps or consumes a physical page");
}

struct WholePage
{
    std::array<std::byte, Provider::page_size> data{};
};
using Pages = ObjectPool<WholePage, Provider, 2>;

void backing_faults()
{
    Provider provider;
    Pages pool(provider);
    provider.fail_allocate = true;
    check(pool.create().status().code() == StatusCode::no_memory && pool.accounting().backing_pages == 0,
          "backing failure publishes no page or object");
    provider.fail_allocate = false;
    provider.available = 1;
    auto first = pool.create();
    check(first.ok(), "backing recovers after ordinary no memory");
    auto *address = pool.resolve(first.value()).value();
    address->data[0] = std::byte{0x12};
    check(pool.create().status().code() == StatusCode::no_memory && pool.resolve(first.value()).value() == address &&
          address->data[0] == std::byte{0x12}, "backing exhaustion preserves prior object");
    provider.available = 2;
    auto second = pool.create();
    check(second.ok(), "second page allocation recovers");
    check(pool.destroy(second.value()).ok(), "empty second page");
    provider.fail_release = true;
    check(pool.trim_empty_pages().code() == StatusCode::io_error && pool.accounting().quarantined_pages == 1 &&
          pool.accounting().backing_pages == 2 && provider.owned() == 2, "uncertain release retains exact page");
    const auto attempts = provider.attempts, releases = provider.releases;
    check(pool.create().status().code() == StatusCode::io_error && provider.attempts == attempts,
          "quarantine refuses future create");
    check(pool.resolve(first.value()).ok() && pool.destroy(first.value()).ok(), "healthy live object survives failed retirement");
    provider.fail_release = false;
    check(pool.trim_empty_pages().code() == StatusCode::io_error && provider.releases == releases + 1 &&
          pool.accounting().backing_pages == 1 && pool.accounting().quarantined_pages == 1,
          "trim releases healthy page but never retries quarantined token");
    check(pool.trim_empty_pages().code() == StatusCode::io_error && provider.releases == releases + 1,
          "quarantine remains sticky without double release");
}

void provider_validation()
{
    for (auto invalid : {Provider::Invalid::short_page, Provider::Invalid::null_page,
                         Provider::Invalid::unaligned, Provider::Invalid::wrap})
        for (bool failed_release : {false, true})
        {
            Provider provider;
            provider.invalid = invalid;
            provider.fail_release = failed_release;
            Pages pool(provider);
            check(pool.create().status().code() == StatusCode::fault && !pool.accounting().live_objects,
                  "invalid supervisor byte span is never dereferenced");
            check(provider.releases == 1 && pool.accounting().quarantined_pages == usize(failed_release) &&
                  provider.owned() == usize(failed_release), "invalid storage lease returned or retained exactly");
            check(pool.trim_empty_pages().code() == StatusCode::fault && provider.releases == 1,
                  "failed invalid lease release never retried");
        }
    Provider alias;
    Pages pool(alias);
    const auto first = pool.create();
    check(first.ok(), "alias setup");
    auto *pointer = pool.resolve(first.value()).value();
    pointer->data[99] = std::byte{0xa1};
    alias.invalid = Provider::Invalid::alias;
    check(pool.create().status().code() == StatusCode::fault && alias.releases == 0 &&
          pool.accounting().quarantined_pages == 1 && pointer->data[99] == std::byte{0xa1},
          "duplicate backing cannot overwrite or release live object");
    check(pool.destroy(first.value()).ok(), "object on original lease can drain");
    // The alias is a provider protocol violation. Both lease records remain
    // retained until every object has drained, then only the healthy one trims.
    check(pool.trim_empty_pages().code() == StatusCode::fault && alias.releases == 1,
          "provider alias quarantine stays explicit");
}

void alignment_and_cleansing()
{
    struct alignas(4096) Aligned { std::byte bytes[4096]; };
    Provider provider;
    ObjectPool<Aligned, Provider, 1> pool(provider);
    auto first = pool.create();
    check(first.ok(), "page alignment type supported");
    auto *p = pool.resolve(first.value()).value();
    check(reinterpret_cast<uptr>(p) % 4096 == 0, "maximum supported alignment");
    for (auto &byte : p->bytes) { check(byte == std::byte{}, "uninitialized trivial members cleared"); byte = std::byte{0xff}; }
    check(pool.destroy(first.value()).ok(), "aligned destroy");
    for (auto byte : provider.backing[0]) check(byte == std::byte{}, "destroyed object bytes cleared");
    auto second = pool.create();
    check(second.ok() && second.value().generation != first.value().generation, "aligned slot reuse");
    for (auto byte : pool.resolve(second.value()).value()->bytes) check(byte == std::byte{}, "cross-owner contents zero");
    check(pool.destroy(second.value()).ok() && pool.trim_empty_pages().ok(), "aligned cleanup");
}

struct Reentrant
{
    // Defined callbacks avoid instantiating sizeof(Reentrant) before completion.
    void *context;
    void (*callback)(void *);
    Reentrant(void *owner, void (*run)(void *)) noexcept : context(owner), callback(run) { callback(context); }
    ~Reentrant() noexcept { callback(context); }
};
using ReentryPool = ObjectPool<Reentrant, Provider, 1>;
struct ReentryContext { ReentryPool *pool; ObjectHandle handle{}; usize calls{}; };
void reenter(void *opaque)
{
    auto &context = *static_cast<ReentryContext *>(opaque);
    ++context.calls;
    check(context.pool->create(&context, reenter).status().code() == StatusCode::busy,
          "constructor/destructor cannot recursively allocate same slot");
    check(context.pool->destroy(context.handle).code() == StatusCode::busy &&
          context.pool->resolve(context.handle).status().code() == StatusCode::busy &&
          context.pool->trim_empty_pages().code() == StatusCode::busy,
          "all mutation callbacks reject reentrant object access");
}
void reentrancy()
{
    Provider provider;
    ReentryPool pool(provider);
    ReentryContext context{&pool};
    const auto created = pool.create(&context, reenter);
    check(created.ok() && context.calls == 1, "outer construction succeeds once");
    context.handle = created.value();
    check(pool.destroy(created.value()).ok() && context.calls == 2, "outer destruction succeeds once");
    check(pool.trim_empty_pages().ok() && !provider.owned(), "reentry test cleanup");
}
} // namespace

int main()
{
    lifecycle();
    refusals();
    backing_faults();
    provider_validation();
    alignment_and_cleansing();
    reentrancy();
    std::printf("PASS object-pool checks=%zu scope=hosted-model guest=0 smp=0\n", checks);
}
