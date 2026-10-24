#include "ok/abi/linux_stat.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace ok;
using namespace ok::abi::linux_user;
static usize checks;
static void check(bool value) { ++checks; if (!value) { std::fprintf(stderr,"check %zu failed\n",checks); std::abort(); } }
static posix::FileStatus input()
{
    posix::FileStatus s{};
    s.type=fs::NodeType::regular; s.mode=0106754; s.uid=0x89abcdef; s.gid=0xfedcba98;
    s.link_count=0xf1234567; s.block_size=4096; s.blocks=0x123456789abcdef;
    s.size=0x456789ab; s.attributes.inode=0xfedcba9876543210;
    s.attributes.device={0xabc,0xdef01}; s.attributes.valid=63;
    s.attributes.access={-1,123456789,fs::TimestampSource::readonly_ext4};
    s.attributes.modification={2208988800,987654321,fs::TimestampSource::readonly_ext4};
    s.attributes.change={-2147483648,0,fs::TimestampSource::readonly_ext4};
    return s;
}
// These are format-only controls for an explicit volatile provider; no epoch
// timestamp, device registration or hardware execution is inferred.
template<Profile P> static void volatile_shared_file()
{
    using C = StatCodec<P>;
    posix::FileStatus base{};
    base.mode = fs::mode_for(fs::NodeType::regular, 0600u);
    base.uid = 1200; base.gid = 1300; base.link_count = 0; base.block_size = 4096;
    base.attributes.inode = 33;
    base.attributes.mark(fs::Attribute::inode); base.attributes.mark(fs::Attribute::blocks512);
    base.provider = posix::FileStatusProvider::volatile_shared_file;
    for (const auto layout : {StatLayout::statx, StatLayout::kernel_stat})
    {
        const usize size = C::size(layout);
        if (!size) continue;
        std::array<u8,256> actual{};
        const auto get = [&](usize offset, usize width) {
            u64 value = 0;
            for (usize index = 0; index < width; ++index) {
                const usize shift = Codec<P>::profile.big_endian ? (width - index - 1) * 8 : index * 8;
                value |= u64{actual[offset + index]} << shift;
            }
            return value;
        };
        for (const usize length : {usize{0}, usize{8192}})
        {
            auto status = base; status.size = length; status.blocks = length / 512;
            actual.fill(0xa5); check(C::encode(status,layout,{actual.data(),size}).ok());
            if (layout == StatLayout::statx) {
                check(get(0,4) == 0x71f && !(get(0,4) & 0xe0));
                check(get(4,4) == 4096 && get(16,4) == 0 && get(20,4) == 1200 && get(24,4) == 1300);
                check(get(28,2) == 0100600 && get(32,8) == 33 && get(40,8) == length && get(48,8) == length / 512);
                check(get(136,4) == 0 && get(140,4) == 0);
                for (usize index=64;index<128;++index) check(actual[index] == 0);
            } else {
                check(get(0,8) == 0 && get(8,8) == 33 && get(48,8) == length && get(64,8) == length / 512);
                if constexpr (P == Profile::x86_64) {
                    check(get(16,8) == 0 && get(24,4) == 0100600 && get(28,4) == 1200 && get(32,4) == 1300);
                    check(get(56,8) == 4096);
                } else {
                    check(get(16,4) == 0100600 && get(20,4) == 0 && get(24,4) == 1200 && get(28,4) == 1300);
                    check(get(56,4) == 4096);
                }
                for (usize index=72;index<120;++index) check(actual[index] == 0);
            }
            for (usize index=size;index<actual.size();++index) check(actual[index] == 0xa5);
        }
        const auto reject = [&](const posix::FileStatus &status) {
            actual.fill(0x5a); const auto before = actual;
            const auto result = C::encode(status,layout,{actual.data(),size});
            check(!result && result.status().code() == StatusCode::unsupported); check(actual == before);
        };
        auto status = base; status.provider = posix::FileStatusProvider::filesystem; reject(status);
        status = input(); status.provider = posix::FileStatusProvider::volatile_shared_file; reject(status);
        status = base; status.provider = static_cast<posix::FileStatusProvider>(255); reject(status);
        status = base; status.type = fs::NodeType::directory; status.mode = fs::mode_for(fs::NodeType::directory,0600); reject(status);
        status = base; status.mode = fs::mode_for(fs::NodeType::regular,0644); reject(status);
        status = base; status.link_count = 1; reject(status);
        status = base; status.block_size = 512; reject(status);
        status = base; status.size = 1; reject(status);
        status = base; status.blocks = 1; reject(status);
        status = base; status.attributes.inode = 0; reject(status);
        for (const u32 fields : {0u,1u,32u,35u,37u,41u,49u,0x80000021u}) {
            status = base; status.attributes.valid = fields; reject(status);
        }
        status = base; status.attributes.device.major = 1; reject(status);
        status = base; status.attributes.device.minor = 1; reject(status);
        for (usize index = 0; index < 3; ++index) {
            for (usize field = 0; field < 3; ++field) {
                status = base;
                auto *time = index == 0 ? &status.attributes.access : index == 1 ? &status.attributes.modification : &status.attributes.change;
                if (field == 0) time->seconds = 1;
                else if (field == 1) time->nanoseconds = 1;
                else time->source = fs::TimestampSource::readonly_ext4;
                reject(status);
            }
        }
    }
}
template<Profile P> static void run(const char *name, const char *directory)
{
    volatile_shared_file<P>();
    using C=StatCodec<P>;
    for(auto layout:{StatLayout::statx,StatLayout::kernel_stat})
    {
        const usize size=C::size(layout);
        if(!size)
        {
            std::array<u8,256> bytes{};bytes.fill(0x5a);const auto initial=bytes;
            check(!C::encode(input(),layout,bytes));check(bytes==initial);continue;
        }
        const auto layout_name=layout==StatLayout::statx?"statx":"kernel";
        char path[1024];std::snprintf(path,sizeof(path),"%s/%s-%s.bin",directory,name,layout_name);
        FILE *file=std::fopen(path,"rb");check(file!=nullptr);
        std::array<u8,256> expected{};check(std::fread(expected.data(),1,size,file)==size);check(std::fgetc(file)==EOF);std::fclose(file);
        std::array<u8,256> actual{};actual.fill(0xa5);
        auto result=C::encode(input(),layout,{actual.data(),size});check(result&&result.value()==size);
        for(usize i=0;i<size;++i)check(actual[i]==expected[i]);
        for(usize i=size;i<actual.size();++i)check(actual[i]==0xa5);
        auto reject=[&](const posix::FileStatus &s, StatusCode code) {
            actual.fill(0x5a);const auto before=actual;
            const auto r=C::encode(s,layout,{actual.data(),size});check(!r&&r.status().code()==code);check(actual==before);
        };
        for(u32 bit:{1u,2u,4u,8u,16u,32u}) {auto s=input();s.attributes.valid&=~bit;reject(s,StatusCode::unsupported);}
        {auto s=input();s.attributes.inode=0;reject(s,StatusCode::unsupported);}
        {auto s=input();s.attributes.device={0,0};reject(s,StatusCode::unsupported);}
        for(auto device:{fs::FilesystemDevice{0,1},fs::FilesystemDevice{1,0}})
        {auto s=input();s.attributes.device=device;check(C::encode(s,layout,{actual.data(),size}).ok());}
        {auto s=input();s.type=fs::NodeType::device;reject(s,StatusCode::unsupported);}
        {auto s=input();s.type=fs::NodeType::directory;reject(s,StatusCode::unsupported);}
        {auto s=input();s.mode|=0x10000;reject(s,StatusCode::overflow);}
        for(usize index=0;index<3;++index)
        {
            auto s=input();auto *time=index==0?&s.attributes.access:index==1?&s.attributes.modification:&s.attributes.change;
            time->nanoseconds=1000000000;reject(s,StatusCode::io_error);
            *time={0,0,fs::TimestampSource::unavailable};reject(s,StatusCode::unsupported);
            *time={0,0,fs::TimestampSource::maintained_ext4};reject(s,StatusCode::unsupported);
        }
        for(i64 seconds:{std::numeric_limits<i64>::min(),i64{-2147483649},i64{-2147483648},i64{-1},i64{0},i64{2147483647},i64{2147483648},i64{2208988800},std::numeric_limits<i64>::max()})
        {
            auto s=input();s.attributes.access={seconds,999999999,fs::TimestampSource::maintained_ext4};
            s.attributes.modification.source=s.attributes.change.source=fs::TimestampSource::maintained_ext4;
            check(C::encode(s,layout,{actual.data(),size}).ok());
            const usize offset=layout==StatLayout::statx?64:72;
            for(usize i=0;i<8;++i)
            {
                const usize significant=Codec<P>::profile.big_endian?7-i:i;
                check(actual[offset+i]==static_cast<u8>(static_cast<u64>(seconds)>>(8*significant)));
            }
        }
        {
            auto s=input();s.blocks=0x8000000000000000;reject(s,StatusCode::overflow);
            s=input();s.block_size=0x80000000;reject(s,StatusCode::overflow);
            if constexpr(sizeof(usize)==8) {s=input();s.size=static_cast<usize>(0x8000000000000000ull);reject(s,StatusCode::overflow);}
            s=input();s.size=std::numeric_limits<usize>::max() >> (sizeof(usize)==8?1:0);
            s.blocks=0x7fffffffffffffff;s.block_size=0x7fffffff;
            check(C::encode(s,layout,{actual.data(),size}).ok());
            s.size=0;s.blocks=0;s.block_size=0;
            check(C::encode(s,layout,{actual.data(),size}).ok());
        }
        if(layout==StatLayout::kernel_stat)
        {
            auto s=input();s.attributes.device.major=0x1000;reject(s,StatusCode::overflow);
            s=input();s.attributes.device.minor=0x100000;reject(s,StatusCode::overflow);
            s=input();s.blocks=0x8000000000000000;reject(s,StatusCode::overflow);
            if constexpr(P!=Profile::x86_64) {s=input();s.block_size=0x80000000;reject(s,StatusCode::overflow);}
        }
        for(usize bad:{usize{0},size-1,size+1})
        {
            std::array<u8,257> buffer{};buffer.fill(0x65);const auto before=buffer;
            check(!C::encode(input(),layout,{buffer.data(),bad}));check(buffer==before);
        }
        for(auto type:{fs::NodeType::directory,fs::NodeType::symlink})
        {
            auto s=input();s.type=type;s.mode=fs::mode_for(type,0755);s.link_count=0;
            check(C::encode(s,layout,{actual.data(),size}).ok());
        }
    }
    std::array<u8,256> invalid{};invalid.fill(0x5a);const auto before=invalid;
    check(!C::encode(input(),static_cast<StatLayout>(255),invalid));check(invalid==before);
}
int main(int argc,char **argv)
{
    check(argc==2);
    run<Profile::i386>("i386",argv[1]);run<Profile::x86_64>("x86_64",argv[1]);
    run<Profile::aarch64>("aarch64",argv[1]);run<Profile::arm32>("arm32",argv[1]);
    run<Profile::rv64>("rv64",argv[1]);run<Profile::rv32>("rv32",argv[1]);
    run<Profile::loongarch64>("loongarch64",argv[1]);run<Profile::mips>("mips",argv[1]);
    run<Profile::mips64>("mips64",argv[1]);run<Profile::ppc>("ppc",argv[1]);
    std::printf("PASS format-only Linux stat codec profiles=10 checks=%zu guest=0 adapter=0\n",checks);
}
