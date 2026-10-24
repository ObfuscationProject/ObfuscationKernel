#include "ok/arch/execution.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>

using namespace ok;
using namespace ok::arch::execution;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "mapping-view:%d: %s\n", __LINE__, #value); std::abort(); } } while (false)

int main()
{
    std::array<PageMapping, max_native_mappings> records{};
    MappingView empty;
    CHECK(empty.valid() && empty.empty());

    MappingView full{records};
    CHECK(full.valid() && full.size() == max_native_mappings);
    CHECK(&full[0] == records.data() && &full[max_native_mappings - 1] == &records.back());

    MappingView segmented;
    segmented.chunks[0] = std::span<const PageMapping>{records.data(), 2};
    segmented.chunks[1] = std::span<const PageMapping>{records.data() + 2, 1};
    segmented.count = 3;
    CHECK(segmented.valid() && &segmented[2] == &records[2]);

    segmented.count = 4;
    CHECK(!segmented.valid()); // A backend must not reach operator[3].
    segmented.count = 2;
    CHECK(!segmented.valid()); // An extra record must not be silently ignored.
    segmented.count = max_native_mappings + 1;
    CHECK(!segmented.valid());
    segmented.count = std::numeric_limits<usize>::max();
    CHECK(!segmented.valid());

    MappingView hidden{std::span<const PageMapping>{records.data(), 1}};
    hidden.count = 0;
    CHECK(!hidden.valid());

    std::puts("PASS mapping view: exact segmented count, too few/many records, over-limit count and overflow-safe rejection; hosted=1 guest=0");
}
