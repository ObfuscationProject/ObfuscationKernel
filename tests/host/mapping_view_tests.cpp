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
    std::array<PageMapping, 64> records{};
    for (usize index = 0; index < records.size(); ++index)
        records[index].virtual_address = user_begin + index * 2 * page_size;
    MappingView empty;
    CHECK(empty.valid() && empty.empty() && !empty.find(user_begin));

    MappingView full{records};
    CHECK(full.valid() && full.size() == records.size());
    CHECK(&full[0] == records.data() && &full[records.size() - 1] == &records.back());
    CHECK(full.begin() == records.data() && full.end() == records.data() + records.size());
    // Lookup is by page: interior addresses resolve, holes and the window end do not.
    CHECK(full.find(user_begin + 2 * page_size + 17) == &records[1]);
    CHECK(full.find(records.back().virtual_address + page_size - 1) == &records.back());
    CHECK(!full.find(user_begin + page_size) && !full.find(records.back().virtual_address + page_size));

    // Order is part of validity: duplicates and descending records are refused.
    records[5].virtual_address = records[4].virtual_address;
    CHECK(!full.valid());
    records[5].virtual_address = records[4].virtual_address - page_size;
    CHECK(!full.valid());
    records[5].virtual_address = user_begin + 10 * page_size;
    CHECK(full.valid());

    MappingView absent{static_cast<const PageMapping *>(nullptr), 1};
    CHECK(!absent.valid());
    MappingView over{records.data(), user_window_pages + 1};
    CHECK(!over.valid());
    over.count = std::numeric_limits<usize>::max();
    CHECK(!over.valid());

    std::puts("PASS mapping view: contiguous records, ascending order, page lookup, absent and over-window rejection; hosted=1 guest=0");
}
