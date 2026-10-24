#include "ok/fs/fat_fs.hpp"
namespace ok::fs
{
namespace
{
u16 r16(const std::byte *p)
{
    return std::to_integer<u16>(p[0]) | (std::to_integer<u16>(p[1]) << 8);
}
u32 r32(const std::byte *p)
{
    return r16(p) | (static_cast<u32>(r16(p + 2)) << 16);
}
u64 r64(const std::byte *p)
{
    return r32(p) | (static_cast<u64>(r32(p + 4)) << 32);
}
void w16(std::byte *p, u16 x)
{
    p[0] = std::byte(x);
    p[1] = std::byte(x >> 8);
}
void w32(std::byte *p, u32 x)
{
    w16(p, static_cast<u16>(x));
    w16(p + 2, static_cast<u16>(x >> 16));
}
void w64(std::byte *p, u64 x)
{
    w32(p, static_cast<u32>(x));
    w32(p + 4, static_cast<u32>(x >> 32));
}
Status corrupt()
{
    return Status::io_error("invalid or unsupported FAT directory/allocation structure");
}
char upper(char c)
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}
bool same(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (usize i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i]))
            return false;
    return true;
}
bool name_valid(std::string_view n)
{
    if (n.empty() || n.size() >= max_path_segment || n == "." || n == ".." || n.back() == '.' || n.back() == ' ')
        return false;
    for (char c : n)
        if (c < 32 || c > 126 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            return false;
    return true;
}
Result<FixedString<13>> short_name(const std::byte *entry)
{
    std::array<char, 13> name{};
    usize length = 0;
    for (usize part = 0; part < 2; ++part)
    {
        const usize first = part ? 8 : 0, end = part ? 11 : 8;
        if (part && entry[first] != std::byte{' '})
            name[length++] = '.';
        for (usize at = first; at < end && entry[at] != std::byte{' '}; ++at)
        {
            const auto ch = std::to_integer<u8>(entry[at]);
            if (ch < 32 || ch > 126)
                return corrupt();
            name[length++] = static_cast<char>(
                (std::to_integer<u8>(entry[12]) & (part ? 16 : 8)) && ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        }
    }
    Result<FixedString<13>> result{std::in_place};
    if (auto status = result.value().assign({name.data(), length}); !status)
        return status;
    return result;
}
u8 short_sum(const std::byte *p)
{
    u8 x = 0;
    for (usize i = 0; i < 11; ++i)
        x = static_cast<u8>(((x & 1) ? 128 : 0) + (x >> 1) + std::to_integer<u8>(p[i]));
    return x;
}
u16 set_sum(std::span<const std::byte> bytes)
{
    u16 sum = 0;
    for (usize i = 0; i < bytes.size(); ++i)
        if (i != 2 && i != 3)
            sum = static_cast<u16>(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + std::to_integer<u8>(bytes[i]));
    return sum;
}
u16 name_hash(std::string_view name)
{
    u16 sum = 0;
    for (char c : name)
    {
        sum = static_cast<u16>(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + static_cast<u8>(upper(c)));
        sum = static_cast<u16>(((sum & 1) ? 0x8000 : 0) + (sum >> 1));
    }
    return sum;
}
constexpr std::array<usize, 13> lfn_offsets{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
} // namespace
FatFileSystem::Guard::Guard(FatFileSystem &owner) : fs(owner)
{
    smp::ScopedSpinLock guard(fs.lock_);
    if (fs.busy_)
        status = Status::busy("FAT operation already active");
    else
    {
        fs.busy_ = true;
        status = {};
    }
}
FatFileSystem::Guard::~Guard()
{
    if (status)
    {
        smp::ScopedSpinLock guard(fs.lock_);
        fs.busy_ = false;
    }
}
bool FatFileSystem::bit(u32 c) const
{
    return owners_[(c - 2) / 8] & (1u << ((c - 2) % 8));
}
void FatFileSystem::bit(u32 c, bool v)
{
    auto &b = owners_[(c - 2) / 8];
    const auto mask = static_cast<u8>(1u << ((c - 2) % 8));
    b = v ? static_cast<u8>(b | mask) : static_cast<u8>(b & ~mask);
}
Status FatFileSystem::read_sector(u64 s, Sector &out) const
{
    if (!device_ || s >= info_.total_sectors)
        return corrupt();
    return device_->read_blocks(s, out);
}
Status FatFileSystem::write_sector(u64 s, const Sector &in)
{
    if (!writable_ || failed_)
        return Status::denied("FAT writes disabled");
    if (!device_ || s >= info_.total_sectors)
        return corrupt();
    ++write_sequence_;
    auto result = device_->write_blocks(s, in);
    if (!result)
        failed_ = true;
    return result;
}
Result<u64> FatFileSystem::cluster_sector(u32 c) const
{
    if (c < 2 || c - 2 >= info_.cluster_count)
        return corrupt();
    return static_cast<u64>(info_.cluster_heap_offset) + static_cast<u64>(c - 2) * info_.sectors_per_cluster;
}
Result<u32> FatFileSystem::fat(u32 c) const
{
    if (c >= info_.cluster_count + 2)
        return corrupt();
    Sector s{};
    const u64 at = static_cast<u64>(c) * 4;
    auto status = read_sector(info_.fat_offset + at / 512, s);
    if (!status)
        return status;
    u32 value = r32(s.data() + at % 512);
    if (info_.variant == FatVariant::fat32)
        value &= 0x0fffffff;
    for (u32 copy = 1; copy < info_.fat_count; ++copy)
    {
        Sector other{};
        status = read_sector(info_.fat_offset + static_cast<u64>(copy) * info_.sectors_per_fat + at / 512, other);
        if (!status)
            return status;
        if ((r32(other.data() + at % 512) & 0x0fffffff) != value)
            return corrupt();
    }
    return value;
}
Status FatFileSystem::set_fat(u32 c, u32 value)
{
    if (c >= info_.cluster_count + 2)
        return corrupt();
    const u64 at = static_cast<u64>(c) * 4;
    for (u32 copy = 0; copy < info_.fat_count; ++copy)
    {
        Sector s{};
        const u64 sector = info_.fat_offset + static_cast<u64>(copy) * info_.sectors_per_fat + at / 512;
        if (auto status = read_sector(sector, s); !status)
            return status;
        if (info_.variant == FatVariant::fat32)
            value = (r32(s.data() + at % 512) & 0xf0000000) | (value & 0x0fffffff);
        w32(s.data() + at % 512, value);
        if (auto status = write_sector(sector, s); !status)
            return status;
    }
    return {};
}
Result<FatFileSystem::Chain> FatFileSystem::chain(u32 first, bool contiguous, u64 bytes) const
{
    Result<Chain> checked{std::in_place};
    auto &result = checked.value();
    if (!first)
    {
        if (bytes)
            return corrupt();
        return checked;
    }
    const u64 cluster_bytes = static_cast<u64>(info_.sectors_per_cluster) * 512;
    const u64 count = (bytes + cluster_bytes - 1) / cluster_bytes;
    if (contiguous && (count > max_chain || !count || first < 2 || first - 2 >= info_.cluster_count ||
                       count > info_.cluster_count - (first - 2)))
        return corrupt();
    u32 c = first;
    for (usize i = 0; i < max_chain; ++i)
    {
        if (c < 2 || c - 2 >= info_.cluster_count)
            return corrupt();
        for (usize j = 0; j < result.count; ++j)
            if (result.values[j] == c)
                return corrupt();
        result.values[result.count++] = c;
        if (contiguous)
        {
            if (result.count == count)
                return checked;
            ++c;
            continue;
        }
        auto next = fat(c);
        if (!next)
            return next.status();
        const u32 eoc = info_.variant == FatVariant::fat32 ? 0x0ffffff8 : 0xfffffff8;
        if (next.value() >= eoc)
            return checked;
        c = next.value();
    }
    return Status::overflow("FAT chain exceeds bounded profile");
}
Status FatFileSystem::claim(const Chain &c)
{
    for (usize i = 0; i < c.count; ++i)
    {
        if (bit(c.values[i]))
            return corrupt();
        auto used = allocated(c.values[i]);
        if (!used || !used.value())
            return corrupt();
        bit(c.values[i], true);
    }
    return {};
}
Result<bool> FatFileSystem::allocated(u32 c) const
{
    if (c < 2 || c - 2 >= info_.cluster_count)
        return corrupt();
    if (info_.variant == FatVariant::fat32)
    {
        auto v = fat(c);
        return v ? Result<bool>{v.value() != 0} : Result<bool>{v.status()};
    }
    auto map = chain(bitmap_);
    if (!map)
        return map.status();
    const u32 at = (c - 2) / 8, cluster_bytes = info_.sectors_per_cluster * 512;
    if (at >= bitmap_bytes_ || at / cluster_bytes >= map.value().count)
        return corrupt();
    auto base = cluster_sector(map.value().values[at / cluster_bytes]);
    if (!base)
        return base.status();
    Sector s{};
    if (auto st = read_sector(base.value() + (at % cluster_bytes) / 512, s); !st)
        return st;
    return (std::to_integer<u8>(s[at % 512]) & (1u << ((c - 2) % 8))) != 0;
}
Status FatFileSystem::set_allocated(u32 c, bool used)
{
    if (info_.variant == FatVariant::fat32)
        return used ? Status{} : set_fat(c, 0);
    auto map = chain(bitmap_);
    if (!map)
        return map.status();
    const u32 at = (c - 2) / 8, cluster_bytes = info_.sectors_per_cluster * 512;
    if (at >= bitmap_bytes_ || at / cluster_bytes >= map.value().count)
        return corrupt();
    auto base = cluster_sector(map.value().values[at / cluster_bytes]);
    if (!base)
        return base.status();
    Sector s{};
    const u64 sector = base.value() + (at % cluster_bytes) / 512;
    if (auto st = read_sector(sector, s); !st)
        return st;
    auto value = std::to_integer<u8>(s[at % 512]);
    const auto mask = static_cast<u8>(1u << ((c - 2) % 8));
    s[at % 512] = std::byte(used ? static_cast<u8>(value | mask) : static_cast<u8>(value & ~mask));
    return write_sector(sector, s);
}
Result<u32> FatFileSystem::allocate_cluster()
{
    for (u32 c = 2; c < info_.cluster_count + 2; ++c)
    {
        if (bit(c))
            continue;
        auto in_use = allocated(c);
        if (!in_use)
            return in_use.status();
        if (in_use.value())
            continue;
        auto base = cluster_sector(c);
        if (!base)
            return base.status();
        Sector zero{};
        for (u32 s = 0; s < info_.sectors_per_cluster; ++s)
            if (auto st = write_sector(base.value() + s, zero); !st)
                return st;
        if (auto st = set_fat(c, info_.variant == FatVariant::fat32 ? 0x0fffffff : 0xffffffff); !st)
            return st;
        if (auto st = set_allocated(c, true); !st)
            return st;
        bit(c, true);
        return c;
    }
    return Status::no_memory("FAT volume has no free cluster");
}
Status FatFileSystem::free_chain(u32 first, bool contiguous, u64 bytes)
{
    auto list = chain(first, contiguous, bytes);
    if (!list)
        return list.status();
    for (usize i = 0; i < list.value().count; ++i)
    {
        const auto c = list.value().values[i];
        if (!bit(c))
            return corrupt();
        if (auto st = set_fat(c, 0); !st)
            return st;
        if (auto st = set_allocated(c, false); !st)
            return st;
        bit(c, false);
    }
    return {};
}
Status FatFileSystem::entry(const FatNode &directory, u32 slot, Entry &out) const
{
    auto c = chain(directory.first_, directory.contiguous_, directory.size_);
    if (!c)
        return c.status();
    const u64 at = static_cast<u64>(slot) * 32, cb = static_cast<u64>(info_.sectors_per_cluster) * 512;
    if (at / cb >= c.value().count)
        return Status::not_found("directory slot beyond chain");
    auto base = cluster_sector(c.value().values[at / cb]);
    if (!base)
        return base.status();
    Sector s{};
    if (auto st = read_sector(base.value() + (at % cb) / 512, s); !st)
        return st;
    for (usize i = 0; i < 32; ++i)
        out[i] = s[(at % 512) + i];
    return {};
}
Status FatFileSystem::put_entry(const FatNode &directory, u32 slot, const Entry &in)
{
    auto c = chain(directory.first_, directory.contiguous_, directory.size_);
    if (!c)
        return c.status();
    const u64 at = static_cast<u64>(slot) * 32, cb = static_cast<u64>(info_.sectors_per_cluster) * 512;
    if (at / cb >= c.value().count)
        return corrupt();
    auto base = cluster_sector(c.value().values[at / cb]);
    if (!base)
        return base.status();
    const u64 sector = base.value() + (at % cb) / 512;
    Sector s{};
    if (auto st = read_sector(sector, s); !st)
        return st;
    for (usize i = 0; i < 32; ++i)
        s[(at % 512) + i] = in[i];
    return write_sector(sector, s);
}
Result<u32> FatFileSystem::lookup(const FatNode &directory, std::string_view name) const
{
    if (!directory.live_ || !directory.directory_)
        return Status::not_directory();
    const u32 parent = static_cast<u32>(&directory - nodes_.data());
    if (name == ".")
        return parent;
    if (name == "..")
        return directory.parent_;
    for (u32 i = 1; i < used_; ++i)
        if (nodes_[i].live_ && nodes_[i].parent_ == parent &&
            (same(name, nodes_[i].name_.view()) || (!nodes_[i].alias_.empty() && same(name, nodes_[i].alias_.view()))))
            return i;
    return Status::not_found();
}
Result<u32> FatFileSystem::add(FatNode &directory, std::string_view name, u32 slot, u32 slots, u32 first, u64 size,
                               u64 valid, bool is_directory, bool contiguous, bool readonly)
{
    if (!name_valid(name) || valid > size || used_ == max_nodes || lookup(directory, name))
        return corrupt();
    if (size > ~usize{0} || size > static_cast<u64>(max_chain) * info_.sectors_per_cluster * 512)
        return Status::overflow("FAT file exceeds supported address/chain profile");
    auto list = chain(first, contiguous, size);
    if (!list)
        return list.status();
    if ((is_directory && !first) || static_cast<u64>(list.value().count) * info_.sectors_per_cluster * 512 < size)
        return corrupt();
    if (auto st = claim(list.value()); !st)
        return st;
    auto &node = nodes_[used_];
    node.fs_ = this;
    node.parent_ = static_cast<u32>(&directory - nodes_.data());
    node.slot_ = slot;
    node.slots_ = slots;
    node.first_ = first;
    node.size_ = size;
    node.valid_ = valid;
    node.directory_ = is_directory;
    node.contiguous_ = contiguous;
    node.readonly_ = readonly;
    node.live_ = true;
    if (auto st = node.name_.assign(name); !st)
        return st;
    return static_cast<u32>(used_++);
}
Status FatFileSystem::scan_fat(FatNode &dir)
{
    std::array<char, 260> longname{};
    u32 start = 0, lfn_count = 0, expected = 0;
    u8 checksum = 0;
    for (u32 slot = 0; slot < 65536; ++slot)
    {
        Entry e{};
        auto st = entry(dir, slot, e);
        if (st.code() == StatusCode::not_found)
            return {};
        if (!st)
            return st;
        const auto first = std::to_integer<u8>(e[0]);
        if (!first)
            return {};
        if (first == 0xe5)
        {
            expected = lfn_count = 0;
            continue;
        }
        const auto attr = std::to_integer<u8>(e[11]);
        if (attr == 0x0f)
        {
            const u32 order = first & 31;
            if (first & 0x40)
            {
                if (!order || order > 20)
                    return corrupt();
                longname.fill(0);
                lfn_count = expected = order;
                start = slot;
                checksum = std::to_integer<u8>(e[13]);
            }
            if (!expected || order != expected || std::to_integer<u8>(e[13]) != checksum || e[12] != std::byte{} ||
                r16(e.data() + 26))
                return corrupt();
            for (usize i = 0; i < 13; ++i)
            {
                const auto ch = r16(e.data() + lfn_offsets[i]);
                if (ch > 127 && ch != 0xffff)
                    return Status::unsupported("non-ASCII FAT name");
                longname[(order - 1) * 13 + i] = ch == 0xffff ? 0 : static_cast<char>(ch);
            }
            --expected;
            continue;
        }
        if (attr & 8)
        {
            if (lfn_count)
                return corrupt();
            continue;
        }
        if (attr & 0xc0)
            return corrupt();
        auto alias = short_name(e.data());
        if (!alias)
            return alias.status();
        std::array<char, 32> name{};
        usize count = 0;
        u32 slots = 1, firstslot = slot;
        if (lfn_count)
        {
            if (expected || short_sum(e.data()) != checksum)
                return corrupt();
            while (count < longname.size() && longname[count])
                ++count;
            if (count >= name.size())
                return Status::unsupported("FAT name exceeds VFS component bound");
            for (usize i = 0; i < count; ++i)
                name[i] = longname[i];
            slots = lfn_count + 1;
            firstslot = start;
        }
        else
        {
            count = alias.value().view().size();
            for (usize i = 0; i < count; ++i)
                name[i] = alias.value().view()[i];
        }
        lfn_count = expected = 0;
        const std::string_view n{name.data(), count};
        if (n == "." || n == "..")
            continue;
        if (lookup(dir, alias.value().view()))
            return corrupt();
        const u32 cluster = (static_cast<u32>(r16(e.data() + 20)) << 16) | r16(e.data() + 26);
        const u64 size = (attr & 16) ? 0 : r32(e.data() + 28);
        auto added = add(dir, n, firstslot, slots, cluster, size, size, attr & 16, false, attr & 1);
        if (!added)
            return added.status();
        nodes_[added.value()].alias_ = alias.value();
    }
    return Status::overflow("FAT directory entry limit");
}
Status FatFileSystem::scan_exfat(FatNode &dir)
{
    for (u32 slot = 0; slot < 65536; ++slot)
    {
        Entry primary{};
        auto st = entry(dir, slot, primary);
        if (st.code() == StatusCode::not_found)
            return {};
        if (!st)
            return st;
        const u8 type = std::to_integer<u8>(primary[0]);
        if (!type)
            return {};
        if (!(type & 0x80))
            continue;
        if (type != 0x85)
        {
            if ((type == 0x81 || type == 0x82 || type == 0x83) && &dir == &nodes_[0])
                continue;
            return Status::unsupported("unknown active exFAT directory entry");
        }
        const u8 secondaries = std::to_integer<u8>(primary[1]);
        if (secondaries < 2 || secondaries > 18)
            return corrupt();
        std::array<std::byte, 608> set{};
        for (usize i = 0; i < 32; ++i)
            set[i] = primary[i];
        for (u32 i = 1; i <= secondaries; ++i)
        {
            Entry e{};
            if (auto s = entry(dir, slot + i, e); !s)
                return s;
            for (usize j = 0; j < 32; ++j)
                set[i * 32 + j] = e[j];
        }
        if (r16(set.data() + 2) != set_sum(std::span<const std::byte>{set}.first((secondaries + 1) * 32)) ||
            set[32] != std::byte{0xc0})
            return corrupt();
        const auto *stream = set.data() + 32;
        const u8 flags = std::to_integer<u8>(stream[1]), len = std::to_integer<u8>(stream[3]);
        if ((flags & ~3u) || !(flags & 1) || !len || len >= max_path_segment || secondaries != 1 + (len + 14) / 15)
            return Status::unsupported("exFAT stream or name exceeds supported profile");
        std::array<char, max_path_segment> name{};
        for (u32 i = 0; i < len; ++i)
        {
            const u32 offset = 64 + (i / 15) * 32;
            if (set[offset] != std::byte{0xc1})
                return corrupt();
            const auto ch = r16(set.data() + offset + 2 + (i % 15) * 2);
            if (ch < 32 || ch > 126)
                return Status::unsupported("non-ASCII exFAT name");
            name[i] = static_cast<char>(ch);
        }
        const std::string_view n{name.data(), len};
        if (r16(stream + 4) != name_hash(n))
            return corrupt();
        const u16 attr = r16(set.data() + 4);
        if (attr & ~0x37u)
            return corrupt();
        auto added = add(dir, n, slot, secondaries + 1, r32(stream + 20), r64(stream + 24), r64(stream + 8), attr & 16,
                         flags & 2, attr & 1);
        if (!added)
            return added.status();
        slot += secondaries;
    }
    return Status::overflow("exFAT directory entry limit");
}
Status FatFileSystem::scan(FatNode &dir)
{
    return info_.variant == FatVariant::fat32 ? scan_fat(dir) : scan_exfat(dir);
}
Status FatFileSystem::initialize_exfat()
{
    for (u32 slot = 0; slot < 65536; ++slot)
    {
        Entry e{};
        auto st = entry(nodes_[0], slot, e);
        if (!st)
            return st;
        if (e[0] == std::byte{})
            break;
        if (e[0] == std::byte{0x81})
        {
            if (bitmap_ || e[1] != std::byte{})
                return corrupt();
            bitmap_ = r32(e.data() + 20);
            const u64 bytes = r64(e.data() + 24);
            if (bytes != (info_.cluster_count + 7u) / 8u)
                return corrupt();
            bitmap_bytes_ = static_cast<u32>(bytes);
        }
        if (e[0] == std::byte{0x82})
        {
            if (upcase_)
                return corrupt();
            upcase_ = r32(e.data() + 20);
            const u64 bytes = r64(e.data() + 24);
            if (!bytes || bytes > 131072 || bytes % 2)
                return corrupt();
            upcase_bytes_ = static_cast<u32>(bytes);
            upcase_checksum_ = r32(e.data() + 4);
        }
    }
    if (!bitmap_ || !upcase_)
        return corrupt();
    auto map = chain(bitmap_), table = chain(upcase_);
    if (!map)
        return map.status();
    if (!table)
        return table.status();
    if (static_cast<u64>(map.value().count) * info_.sectors_per_cluster * 512 < bitmap_bytes_ ||
        static_cast<u64>(table.value().count) * info_.sectors_per_cluster * 512 < upcase_bytes_)
        return corrupt();
    if (auto st = claim(map.value()); !st)
        return st;
    if (auto st = claim(table.value()); !st)
        return st;
    u32 checksum = 0, remaining = upcase_bytes_;
    usize ascii = 0;
    for (usize i = 0; i < table.value().count && remaining; ++i)
    {
        auto base = cluster_sector(table.value().values[i]);
        if (!base)
            return base.status();
        for (u32 j = 0; j < info_.sectors_per_cluster && remaining; ++j)
        {
            Sector s{};
            if (auto st = read_sector(base.value() + j, s); !st)
                return st;
            const usize n = remaining < 512 ? remaining : 512;
            for (usize k = 0; k < n; ++k)
                checksum = ((checksum & 1) ? 0x80000000u : 0) + (checksum >> 1) + std::to_integer<u8>(s[k]);
            for (usize k = 0; k + 1 < n && ascii < 128; k += 2, ++ascii)
                if (r16(s.data() + k) != static_cast<u16>(upper(static_cast<char>(ascii))))
                    return Status::unsupported("exFAT upcase ASCII mapping unsupported");
            remaining -= static_cast<u32>(n);
        }
    }
    return checksum == upcase_checksum_ && ascii == 128 ? Status{} : corrupt();
}
Status FatFileSystem::mount(driver::BlockDevice &device, bool writable)
{
    Guard guard(*this);
    if (!guard.status)
        return guard.status;
    if (mounted_ || device_)
        return Status::busy("FAT provider cannot be rebound");
    auto st = volume_.mount(device);
    if (!st)
        return st;
    auto info = volume_.info();
    if (!info)
        return info.status();
    info_ = info.value();
    if (info_.cluster_count > max_clusters || info_.sectors_per_cluster > 128 ||
        (info_.variant == FatVariant::fat32 && info_.cluster_count < 65525))
        return Status::unsupported("FAT geometry exceeds bounded VFS profile");
    if (writable && !device.geometry().writable)
        return Status::denied("read-only FAT device");
    device_ = &device;
    writable_ = writable;
    Sector boot{};
    if (auto s = read_sector(0, boot); !s)
        return s;
    if (info_.variant == FatVariant::fat32)
    {
        if (r16(boot.data() + 40) & 0x80)
            return Status::unsupported("nonmirrored FAT32 table selection");
        fsinfo_ = r16(boot.data() + 48);
        backup_boot_ = r16(boot.data() + 50);
        if ((fsinfo_ != 0xffff && fsinfo_ >= info_.reserved_sector_count) ||
            (backup_boot_ != 0xffff && backup_boot_ >= info_.reserved_sector_count))
            return corrupt();
        auto flags = fat(1);
        if (!flags)
            return flags.status();
        if (!(flags.value() & 0x08000000) || !(flags.value() & 0x04000000))
            return Status::denied("FAT32 requires clean media");
    }
    else
    {
        if (info_.fat_count != 1 || r16(boot.data() + 104) != 0x100 || r16(boot.data() + 106))
            return Status::unsupported("exFAT version, dirty state or FAT selection unsupported");
        // The dirty and percent-in-use bytes are excluded by the exFAT checksum.
        for (u64 first : {u64{0}, u64{12}})
        {
            u32 checksum = 0;
            for (u64 sector = 0; sector < 11; ++sector)
            {
                Sector bytes{};
                if (auto status = read_sector(first + sector, bytes); !status)
                    return status;
                if (!sector)
                {
                    if (r16(bytes.data() + 106))
                        return Status::denied("exFAT backup is dirty or failed");
                    for (usize at = 64; at < 113; ++at)
                        if (at != 106 && at != 107 && at != 112 && bytes[at] != boot[at])
                            return corrupt();
                }
                for (usize at = 0; at < bytes.size(); ++at)
                {
                    if (!sector && (at == 106 || at == 107 || at == 112))
                        continue;
                    checksum = ((checksum & 1) ? 0x80000000u : 0) + (checksum >> 1) + std::to_integer<u8>(bytes[at]);
                }
            }
            Sector sums{};
            if (auto status = read_sector(first + 11, sums); !status)
                return status;
            for (usize at = 0; at < sums.size(); at += 4)
                if (r32(sums.data() + at) != checksum)
                    return corrupt();
        }
    }
    auto &root = nodes_[0];
    root.fs_ = this;
    root.first_ = info_.root_cluster;
    root.directory_ = root.live_ = true;
    static_cast<void>(root.name_.assign("/"));
    if (info_.variant == FatVariant::exfat)
    {
        if (auto s = initialize_exfat(); !s)
            return s;
    }
    auto rootchain = chain(root.first_);
    if (!rootchain)
        return rootchain.status();
    if (auto s = claim(rootchain.value()); !s)
        return s;
    for (usize i = 0; i < used_; ++i)
        if (nodes_[i].directory_)
        {
            if (auto s = scan(nodes_[i]); !s)
                return s;
        }
    mounted_ = true;
    return {};
}
Status FatFileSystem::set_dirty(bool value)
{
    if (info_.variant == FatVariant::fat32)
    {
        auto flags = fat(1);
        if (!flags)
            return flags.status();
        return set_fat(1, value ? (flags.value() & ~0x08000000u) : (flags.value() | 0x08000000u));
    }
    for (u64 sector : {u64{0}, u64{12}})
    {
        Sector boot{};
        if (auto s = read_sector(sector, boot); !s)
            return s;
        auto flags = r16(boot.data() + 106);
        w16(boot.data() + 106, static_cast<u16>(value ? flags | 2u : flags & ~2u));
        boot[112] = std::byte{0xff}; // Allocation percentage is not maintained by this provider.
        if (auto s = write_sector(sector, boot); !s)
            return s;
    }
    return {};
}
Status FatFileSystem::start_write()
{
    if (!mounted_ || !writable_ || failed_)
        return Status::denied("FAT mutation unavailable");
    if (dirty_)
        return {};
    if (auto st = set_dirty(true); !st)
        return st;
    dirty_ = true;
    auto st = device_->flush();
    if (!st)
        failed_ = true;
    return st;
}
Status FatFileSystem::persist_fat_hints()
{
    if (info_.variant != FatVariant::fat32)
        return {};
    for (u32 s : {static_cast<u32>(fsinfo_), static_cast<u32>(backup_boot_) + fsinfo_})
    {
        if (fsinfo_ == 0xffff || s == 0 || s >= info_.reserved_sector_count)
            continue;
        Sector b{};
        if (auto st = read_sector(s, b); !st)
            return st;
        if (r32(b.data()) != 0x41615252 || r32(b.data() + 484) != 0x61417272 || r32(b.data() + 508) != 0xaa550000)
            return corrupt();
        w32(b.data() + 488, 0xffffffff);
        w32(b.data() + 492, 0xffffffff);
        if (auto st = write_sector(s, b); !st)
            return st;
    }
    return {};
}
Status FatFileSystem::sync()
{
    Guard guard(*this);
    if (!guard.status)
        return guard.status;
    if (!mounted_ || failed_)
        return Status::io_error("FAT sync unavailable");
    if (!dirty_)
        return device_->flush();
    for (usize i = 1; i < used_; ++i)
        if (!nodes_[i].live_ && nodes_[i].references_ && nodes_[i].first_)
            return Status::busy("FAT retained unlinked data prevents clean publication");
    if (auto st = persist_fat_hints(); !st)
    {
        failed_ = true;
        return st;
    }
    if (auto st = device_->flush(); !st)
    {
        failed_ = true;
        return st;
    }
    if (auto st = set_dirty(false); !st)
        return st;
    if (auto st = device_->flush(); !st)
    {
        failed_ = true;
        return st;
    }
    dirty_ = false;
    return {};
}
Status FatFileSystem::extend(FatNode &node, usize wanted)
{
    auto list = chain(node.first_, node.contiguous_, node.size_);
    if (!list)
        return list.status();
    if (wanted > max_chain)
        return Status::overflow("FAT file chain capacity");
    if (list.value().count >= wanted)
        return {};
    if (node.contiguous_)
    {
        for (usize i = 0; i < list.value().count; ++i)
            if (auto st = set_fat(list.value().values[i],
                                  i + 1 < list.value().count ? list.value().values[i + 1] : 0xffffffff);
                !st)
                return st;
        node.contiguous_ = false;
        if (auto st = publish(node); !st)
            return st;
    }
    u32 last = list.value().count ? list.value().values[list.value().count - 1] : 0;
    for (usize i = list.value().count; i < wanted; ++i)
    {
        auto next = allocate_cluster();
        if (!next)
            return next.status();
        if (last)
        {
            if (auto st = set_fat(last, next.value()); !st)
                return st;
        }
        else
            node.first_ = next.value();
        last = next.value();
    }
    return {};
}
Status FatFileSystem::publish(FatNode &node)
{
    if (&node == &nodes_[0] || !node.live_)
        return {};
    auto &parent = nodes_[node.parent_];
    if (info_.variant == FatVariant::fat32)
    {
        Entry e{};
        if (auto st = entry(parent, node.slot_ + node.slots_ - 1, e); !st)
            return st;
        w16(e.data() + 20, static_cast<u16>(node.first_ >> 16));
        w16(e.data() + 26, static_cast<u16>(node.first_));
        w32(e.data() + 28, node.directory_ ? 0 : static_cast<u32>(node.size_));
        return put_entry(parent, node.slot_ + node.slots_ - 1, e);
    }
    if (node.slots_ > 5)
        return corrupt();
    std::array<std::byte, 160> set{};
    for (u32 i = 0; i < node.slots_; ++i)
    {
        Entry e{};
        if (auto st = entry(parent, node.slot_ + i, e); !st)
            return st;
        for (usize j = 0; j < 32; ++j)
            set[i * 32 + j] = e[j];
    }
    auto *stream = set.data() + 32;
    stream[1] = std::byte(node.contiguous_ ? 3 : 1);
    w64(stream + 8, node.valid_);
    w32(stream + 20, node.first_);
    w64(stream + 24, node.size_);
    w16(set.data() + 2, set_sum(std::span<const std::byte>{set}.first(node.slots_ * 32)));
    for (u32 i = node.slots_; i > 0; --i)
    {
        Entry e{};
        for (usize j = 0; j < 32; ++j)
            e[j] = set[(i - 1) * 32 + j];
        if (auto st = put_entry(parent, node.slot_ + i - 1, e); !st)
            return st;
    }
    return {};
}
Result<u32> FatFileSystem::free_slots(FatNode &directory, u32 count)
{
    u32 run = 0;
    for (u32 slot = 0; slot < 65536; ++slot)
    {
        Entry e{};
        auto st = entry(directory, slot, e);
        if (st.code() == StatusCode::not_found)
        {
            auto list = chain(directory.first_, directory.contiguous_, directory.size_);
            if (!list)
                return list.status();
            if (auto s = extend(directory, list.value().count + 1); !s)
                return s;
            if (info_.variant == FatVariant::exfat && &directory != &nodes_[0])
            {
                directory.size_ = directory.valid_ =
                    (list.value().count + 1) * static_cast<u64>(info_.sectors_per_cluster) * 512;
                if (auto s = publish(directory); !s)
                    return s;
            }
            e.fill(std::byte{});
        }
        else if (!st)
            return st;
        const auto first = std::to_integer<u8>(e[0]);
        const bool free = info_.variant == FatVariant::fat32 ? (!first || first == 0xe5) : !(first & 0x80);
        run = free ? run + 1 : 0;
        if (run == count)
            return slot - count + 1;
    }
    return Status::overflow("FAT directory full");
}
Status FatFileSystem::zero(FatNode &node, u64 begin, u64 end)
{
    if (begin > end)
        return corrupt();
    auto c = chain(node.first_, node.contiguous_, node.size_);
    if (!c)
        return c.status();
    const u64 cb = static_cast<u64>(info_.sectors_per_cluster) * 512;
    while (begin < end)
    {
        if (begin / cb >= c.value().count)
            return corrupt();
        auto base = cluster_sector(c.value().values[begin / cb]);
        if (!base)
            return base.status();
        const u64 sector = base.value() + (begin % cb) / 512;
        Sector s{};
        if (begin % 512 || end - begin < 512)
            if (auto st = read_sector(sector, s); !st)
                return st;
        const usize n = static_cast<usize>(end - begin < 512 - begin % 512 ? end - begin : 512 - begin % 512);
        for (usize i = 0; i < n; ++i)
            s[begin % 512 + i] = std::byte{};
        if (auto st = write_sector(sector, s); !st)
            return st;
        begin += n;
    }
    return {};
}
Status FatFileSystem::resize(FatNode &node, u64 size)
{
    if ((!node.live_ && !node.references_) || node.directory_ || node.readonly_)
        return Status::denied("FAT file is not writable");
    const u64 cb = static_cast<u64>(info_.sectors_per_cluster) * 512;
    if (size > 0xffffffffu || size > max_chain * cb)
        return Status::overflow("FAT file size limit");
    if (size == node.size_)
        return {};
    if (auto st = start_write(); !st)
        return st;
    auto list = chain(node.first_, node.contiguous_, node.size_);
    if (!list)
        return list.status();
    const usize wanted = static_cast<usize>((size + cb - 1) / cb);
    if (size > node.size_)
    {
        if (auto st = extend(node, wanted); !st)
            return st;
        const u64 old = node.valid_;
        node.size_ = size;
        if (auto st = zero(node, old, size); !st)
            return st;
        node.valid_ = size;
        return publish(node);
    }
    const u32 oldfirst = node.first_;
    const bool contiguous = node.contiguous_;
    const u64 oldsize = node.size_;
    node.size_ = size;
    node.valid_ = node.valid_ < size ? node.valid_ : size;
    if (!wanted)
        node.first_ = 0;
    if (auto st = publish(node); !st)
        return st;
    if (wanted < list.value().count)
    {
        if (wanted && !contiguous)
            if (auto st = set_fat(list.value().values[wanted - 1],
                                  info_.variant == FatVariant::fat32 ? 0x0fffffff : 0xffffffff);
                !st)
                return st;
        const u32 first = wanted ? list.value().values[wanted] : oldfirst;
        return free_chain(first, contiguous, contiguous ? oldsize - static_cast<u64>(wanted) * cb : 0);
    }
    return {};
}
Result<FileBuffer> FatFileSystem::read(const FatNode &node, u64 offset, usize count) const
{
    if (!node.live_ && !node.references_)
        return Status::not_found();
    if (node.directory_)
        return Status::invalid_argument("cannot read directory bytes");
    Result<FileBuffer> checked{std::in_place};
    auto &out = checked.value();
    if (offset >= node.size_)
        return checked;
    u64 n = node.size_ - offset;
    if (n > count)
        n = count;
    if (n > out.data.size())
        n = out.data.size();
    out.size = static_cast<usize>(n);
    auto c = chain(node.first_, node.contiguous_, node.size_);
    if (!c)
        return c.status();
    const u64 cb = static_cast<u64>(info_.sectors_per_cluster) * 512;
    for (usize copied = 0; copied < out.size;)
    {
        const u64 at = offset + copied;
        if (at >= node.valid_)
            break;
        if (at / cb >= c.value().count)
            return corrupt();
        auto base = cluster_sector(c.value().values[at / cb]);
        if (!base)
            return base.status();
        Sector s{};
        if (auto st = read_sector(base.value() + (at % cb) / 512, s); !st)
            return st;
        usize chunk = 512 - at % 512;
        if (chunk > out.size - copied)
            chunk = out.size - copied;
        if (chunk > node.valid_ - at)
            chunk = static_cast<usize>(node.valid_ - at);
        for (usize i = 0; i < chunk; ++i)
            out.data[copied + i] = s[at % 512 + i];
        copied += chunk;
    }
    return checked;
}
Status FatFileSystem::write(FatNode &node, u64 offset, std::span<const std::byte> bytes)
{
    if ((!node.live_ && !node.references_) || node.directory_ || node.readonly_)
        return Status::denied();
    if (bytes.empty())
        return {};
    if (offset > 0xffffffffu || bytes.size() > 0xffffffffu - offset)
        return Status::overflow();
    if (auto st = start_write(); !st)
        return st;
    const u64 end = offset + bytes.size();
    if (end > node.size_)
        if (auto st = resize(node, end); !st)
            return st;
    if (offset > node.valid_)
        if (auto st = zero(node, node.valid_, offset); !st)
            return st;
    auto c = chain(node.first_, node.contiguous_, node.size_);
    if (!c)
        return c.status();
    const u64 cb = static_cast<u64>(info_.sectors_per_cluster) * 512;
    for (usize copied = 0; copied < bytes.size();)
    {
        const u64 at = offset + copied;
        if (at / cb >= c.value().count)
            return corrupt();
        auto base = cluster_sector(c.value().values[at / cb]);
        if (!base)
            return base.status();
        const u64 sector = base.value() + (at % cb) / 512;
        Sector s{};
        if (auto st = read_sector(sector, s); !st)
            return st;
        usize chunk = 512 - at % 512;
        if (chunk > bytes.size() - copied)
            chunk = bytes.size() - copied;
        for (usize i = 0; i < chunk; ++i)
            s[at % 512 + i] = bytes[copied + i];
        if (auto st = write_sector(sector, s); !st)
            return st;
        copied += chunk;
    }
    if (end > node.valid_)
        node.valid_ = end;
    return publish(node);
}
Result<Node *> FatFileSystem::create(FatNode &parent, std::string_view name, const InitialNodeAttributes &attributes)
{
    if (!parent.live_ || !parent.directory_)
        return Status::not_directory();
    const bool directory = attributes.type == NodeType::directory;
    if ((attributes.type != NodeType::regular && !directory) || attributes.uid || attributes.gid ||
        attributes.mode != (directory ? 0755u : 0644u))
        return Status::unsupported("FAT supports root-owned fixed 0644/0755 attributes");
    if (!name_valid(name))
        return Status::invalid_argument("invalid or out-of-profile FAT name");
    if (lookup(parent, name))
        return Status::already_exists();
    usize index = 1;
    for (; index < used_; ++index)
        if (!nodes_[index].live_ && !nodes_[index].references_ && !nodes_[index].first_ && !nodes_[index].exposed_)
            break;
    if (index == max_nodes)
        return Status::no_memory("FAT live or exposed node capacity");
    if (auto st = start_write(); !st)
        return st;
    const u32 slots = static_cast<u32>(info_.variant == FatVariant::fat32 ? (name.size() + 12) / 13 + 1
                                                                          : (name.size() + 14) / 15 + 2);
    auto free = free_slots(parent, slots);
    if (!free)
        return free.status();
    auto &node = nodes_[index];
    node = FatNode{};
    node.fs_ = this;
    node.parent_ = static_cast<u32>(&parent - nodes_.data());
    node.slot_ = free.value();
    node.slots_ = slots;
    node.directory_ = directory;
    static_cast<void>(node.name_.assign(name));
    if (directory)
    {
        auto c = allocate_cluster();
        if (!c)
            return c.status();
        node.first_ = c.value();
        if (info_.variant == FatVariant::exfat)
            node.size_ = node.valid_ = static_cast<u64>(info_.sectors_per_cluster) * 512;
        else
        {
            Sector s{};
            for (usize entry_index = 0; entry_index < 2; ++entry_index)
            {
                auto *e = s.data() + entry_index * 32;
                for (usize j = 0; j < 11; ++j)
                    e[j] = std::byte{' '};
                e[0] = std::byte{'.'};
                if (entry_index)
                    e[1] = std::byte{'.'};
                e[11] = std::byte{16};
                const u32 cluster = entry_index ? (&parent == &nodes_[0] ? 0 : parent.first_) : node.first_;
                w16(e + 20, static_cast<u16>(cluster >> 16));
                w16(e + 26, static_cast<u16>(cluster));
                w16(e + 16, 0x21);
                w16(e + 18, 0x21);
                w16(e + 24, 0x21);
            }
            auto base = cluster_sector(node.first_);
            if (!base)
                return base.status();
            if (auto st = write_sector(base.value(), s); !st)
                return st;
        }
    }
    if (info_.variant == FatVariant::fat32)
    {
        Entry short_entry{};
        bool unique = false;
        for (u32 serial = 1; serial < 100000 && !unique; ++serial)
        {
            short_entry.fill(std::byte{});
            short_entry[0] = std::byte{'F'};
            u32 number = serial;
            for (usize i = 8; i > 1; --i)
            {
                short_entry[i - 1] = std::byte('0' + number % 10);
                number /= 10;
            }
            short_entry[8] = std::byte{'D'};
            short_entry[9] = std::byte{'A'};
            short_entry[10] = std::byte{'T'};
            auto alias = short_name(short_entry.data());
            if (!alias)
                return alias.status();
            if (same(name, alias.value().view()) || lookup(parent, alias.value().view()))
                continue;
            node.alias_ = alias.value();
            unique = true;
            for (u32 i = 0; i < 65536; ++i)
            {
                Entry candidate{};
                auto st = entry(parent, i, candidate);
                if (st.code() == StatusCode::not_found)
                    break;
                if (!st)
                    return st;
                if (candidate[0] == std::byte{})
                    break;
                if (candidate[0] == std::byte{0xe5} || candidate[11] == std::byte{0x0f})
                    continue;
                bool equal = true;
                for (usize j = 0; j < 11; ++j)
                    equal &= candidate[j] == short_entry[j];
                if (equal)
                {
                    unique = false;
                    break;
                }
            }
        }
        if (!unique)
            return Status::overflow("FAT short alias limit");
        short_entry[11] = std::byte(directory ? 16 : 32);
        w16(short_entry.data() + 16, 0x21);
        w16(short_entry.data() + 18, 0x21);
        w16(short_entry.data() + 24, 0x21);
        w16(short_entry.data() + 20, static_cast<u16>(node.first_ >> 16));
        w16(short_entry.data() + 26, static_cast<u16>(node.first_));
        const auto checksum = short_sum(short_entry.data());
        for (u32 i = 0; i < slots - 1; ++i)
        {
            const u32 sequence = slots - 1 - i;
            Entry e{};
            e[0] = std::byte(sequence | (i == 0 ? 0x40 : 0));
            e[11] = std::byte{0x0f};
            e[13] = std::byte(checksum);
            for (usize j = 0; j < 13; ++j)
            {
                const usize index = (sequence - 1) * 13 + j;
                w16(e.data() + lfn_offsets[j], index < name.size()    ? static_cast<u8>(name[index])
                                               : index == name.size() ? 0
                                                                      : 0xffff);
            }
            if (auto st = put_entry(parent, node.slot_ + i, e); !st)
                return st;
        }
        if (auto st = put_entry(parent, node.slot_ + slots - 1, short_entry); !st)
            return st;
    }
    else
    {
        std::array<std::byte, 160> set{};
        set[0] = std::byte{0x85};
        set[1] = std::byte(slots - 1);
        w16(set.data() + 4, directory ? 16 : 32);
        w32(set.data() + 8, 0x00210000);
        w32(set.data() + 12, 0x00210000);
        w32(set.data() + 16, 0x00210000);
        set[32] = std::byte{0xc0};
        set[33] = std::byte{1};
        set[35] = std::byte(name.size());
        w16(set.data() + 36, name_hash(name));
        w64(set.data() + 40, node.valid_);
        w32(set.data() + 52, node.first_);
        w64(set.data() + 56, node.size_);
        for (u32 i = 2; i < slots; ++i)
            set[i * 32] = std::byte{0xc1};
        for (usize i = 0; i < name.size(); ++i)
            w16(set.data() + 64 + (i / 15) * 32 + 2 + (i % 15) * 2, static_cast<u8>(name[i]));
        w16(set.data() + 2, set_sum(std::span<const std::byte>{set}.first(slots * 32)));
        for (u32 i = slots; i > 0; --i)
        {
            Entry e{};
            for (usize j = 0; j < 32; ++j)
                e[j] = set[(i - 1) * 32 + j];
            if (auto st = put_entry(parent, node.slot_ + i - 1, e); !st)
                return st;
        }
    }
    node.live_ = true;
    if (index == used_)
        ++used_;
    return static_cast<Node *>(&node);
}
Status FatFileSystem::remove(FatNode &parent, std::string_view name, bool is_directory)
{
    if (name == "." || name == "..")
        return Status::invalid_argument();
    auto found = lookup(parent, name);
    if (!found)
        return found.status();
    auto &node = nodes_[found.value()];
    if (node.directory_ != is_directory)
        return Status::invalid_argument("FAT unlink type mismatch");
    if (node.readonly_)
        return Status::denied("FAT removal of readonly node is unavailable");
    if (node.directory_)
        for (usize i = 1; i < used_; ++i)
            if (nodes_[i].live_ && nodes_[i].parent_ == found.value())
                return Status::busy("FAT directory not empty");
    if (auto st = start_write(); !st)
        return st;
    for (u32 i = 0; i < node.slots_; ++i)
    {
        const u32 relative = info_.variant == FatVariant::fat32 ? node.slots_ - 1 - i : i;
        Entry e{};
        if (auto st = entry(parent, node.slot_ + relative, e); !st)
            return st;
        e[0] = info_.variant == FatVariant::fat32 ? std::byte{0xe5} : std::byte(std::to_integer<u8>(e[0]) & 0x7f);
        if (auto st = put_entry(parent, node.slot_ + relative, e); !st)
            return st;
    }
    node.live_ = false;
    if (node.references_)
        return {};
    auto status = free_chain(node.first_, node.contiguous_, node.size_);
    if (status)
        node.first_ = 0;
    return status;
}
Metadata FatNode::metadata() const
{
    Metadata m;
    m.type = directory_ ? NodeType::directory : NodeType::regular;
    m.mode = mode_for(m.type, readonly_ ? (directory_ ? 0555u : 0444u) : (directory_ ? 0755u : 0644u));
    m.size = static_cast<usize>(size_);
    m.block_size = fs_ ? fs_->info_.sectors_per_cluster * 512 : 512;
    m.blocks = blocks_for_size(m.size);
    m.link_count = live_ ? 1 : 0;
    return m;
}
Result<Metadata> FatNode::query_metadata() const
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    return live_ || references_ ? Result<Metadata>{metadata()} : Result<Metadata>{Status::not_found()};
}
Result<FileBuffer> FatNode::read(usize offset, usize count) const
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    return fs_->read(*this, offset, count);
}
Status FatNode::write(usize offset, std::span<const std::byte> bytes)
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    const auto before = fs_->write_sequence_;
    auto result = fs_->write(*this, offset, bytes);
    if (!result && fs_->write_sequence_ != before)
        fs_->failed_ = true;
    return result;
}
Status FatNode::truncate(usize size)
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    const auto before = fs_->write_sequence_;
    auto result = fs_->resize(*this, size);
    if (!result && fs_->write_sequence_ != before)
        fs_->failed_ = true;
    return result;
}
Status FatNode::sync()
{
    return fs_ ? fs_->sync() : Status::not_found();
}
Status FatNode::retain()
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    if ((!live_ && !references_) || references_ == ~u32{0})
        return Status::busy();
    ++references_;
    return {};
}
Status FatNode::release()
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    if (!references_)
        return Status::invalid_argument();
    --references_;
    if (!references_ && !live_ && first_)
    {
        const auto before = fs_->write_sequence_;
        auto status = fs_->start_write();
        if (status)
            status = fs_->free_chain(first_, contiguous_, size_);
        if (status)
            first_ = 0;
        else if (fs_->write_sequence_ != before)
            fs_->failed_ = true;
        return status;
    }
    return {};
}
Status FatNode::expose_borrowed_identity()
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    if (!live_ && !references_)
        return Status::not_found();
    exposed_ = true;
    return {};
}
Result<Node *> FatNode::lookup_child(std::string_view name)
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    auto found = fs_->lookup(*this, name);
    if (!found)
        return found.status();
    auto &node = fs_->nodes_[found.value()];
    node.exposed_ = true;
    return static_cast<Node *>(&node);
}
Result<Node *> FatNode::lookup_child_retained(std::string_view name)
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    auto found = fs_->lookup(*this, name);
    if (!found)
        return found.status();
    auto &node = fs_->nodes_[found.value()];
    if (node.references_ == ~u32{0})
        return Status::overflow("FAT reference count exhausted");
    ++node.references_;
    return static_cast<Node *>(&node);
}
Node *FatNode::lookup(std::string_view name)
{
    auto node = lookup_child(name);
    return node ? node.value() : nullptr;
}
Result<Node *> FatNode::create_child(std::string_view name, const InitialNodeAttributes &attributes)
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    const auto before = fs_->write_sequence_;
    auto result = fs_->create(*this, name, attributes);
    if (!result && fs_->write_sequence_ != before)
        fs_->failed_ = true;
    if (result)
        static_cast<FatNode *>(result.value())->exposed_ = true;
    return result;
}
Result<Node *> FatNode::create_child_retained(std::string_view name, const InitialNodeAttributes &attributes)
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    const auto before = fs_->write_sequence_;
    auto result = fs_->create(*this, name, attributes);
    if (!result && fs_->write_sequence_ != before)
        fs_->failed_ = true;
    if (result)
        ++static_cast<FatNode *>(result.value())->references_;
    return result;
}
Status FatNode::create(std::string_view name, NodeType type)
{
    auto made = create_child_retained(name, {type, type == NodeType::directory ? 0755u : 0644u, 0, 0});
    return made ? made.value()->release() : made.status();
}
Status FatNode::remove(std::string_view name, bool directory)
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    const auto before = fs_->write_sequence_;
    auto result = fs_->remove(*this, name, directory);
    if (!result && fs_->write_sequence_ != before)
        fs_->failed_ = true;
    return result;
}
Result<DirectoryListing> FatNode::list(DirectoryReadIntent) const
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    if (!live_ || !directory_)
        return Status::not_directory();
    Result<DirectoryListing> checked{std::in_place};
    auto &out = checked.value();
    const u32 parent = static_cast<u32>(this - fs_->nodes_.data());
    for (usize i = 1; i < fs_->used_; ++i)
    {
        const auto &child = fs_->nodes_[i];
        if (!child.live_ || child.parent_ != parent)
            continue;
        if (out.count == out.entries.size())
            return Status::overflow("FAT directory listing capacity");
        auto &e = out.entries[out.count++];
        e.name = child.name_;
        e.metadata = child.metadata();
    }
    return checked;
}
Result<FixedString<96>> FatNode::directory_path(const Node &root) const
{
    if (!fs_)
        return Status::not_found();
    FatFileSystem::Guard guard(*fs_);
    if (!guard.status)
        return guard.status;
    if (&root != &fs_->nodes_[0] || !live_ || !directory_)
        return Status::invalid_argument();
    std::array<usize, FatFileSystem::max_nodes> path{};
    usize count = 0, index = static_cast<usize>(this - fs_->nodes_.data());
    while (index)
    {
        if (count == path.size())
            return corrupt();
        path[count++] = index;
        index = fs_->nodes_[index].parent_;
    }
    std::array<char, 96> bytes{};
    usize length = 0;
    bytes[length++] = '/';
    while (count)
    {
        const auto name = fs_->nodes_[path[--count]].name_.view();
        if (name.size() + length + (count ? 1 : 0) >= bytes.size())
            return Status::overflow();
        for (char c : name)
            bytes[length++] = c;
        if (count)
            bytes[length++] = '/';
    }
    FixedString<96> out;
    if (auto st = out.assign({bytes.data(), length}); !st)
        return st;
    return out;
}
} // namespace ok::fs
