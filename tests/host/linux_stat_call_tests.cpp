// Hosted adapter policy and callback-order models; not a syscall execution test.
#include "ok/abi/linux_stat_call.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace ok;
using namespace ok::abi::linux_user;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); std::abort(); } } while(false)
struct Model
{
    std::string order, name{"/file"};
    posix::FileStatus metadata{};
    Status path_status{}, query_status{}, copy_status{};
    bool writable{true};
    std::array<u8, 300> output{};
    StatRequest queried{};
    std::string queried_name;
    Model()
    {
        output.fill(0xa5);
        metadata.type = fs::NodeType::regular; metadata.mode = 0100644;
        metadata.size = 33; metadata.block_size = 1024; metadata.blocks = 2;
        metadata.link_count = 0; metadata.uid = 1000; metadata.gid = 1000;
        auto &a = metadata.attributes;
        a.inode = 27; a.device = {1, 5};
        for (auto bit : {fs::Attribute::inode, fs::Attribute::device, fs::Attribute::access_time,
             fs::Attribute::modification_time, fs::Attribute::change_time, fs::Attribute::blocks512}) a.mark(bit);
        a.access = {-1, 999999999, fs::TimestampSource::readonly_ext4};
        a.modification = {2208988800, 1234, fs::TimestampSource::readonly_ext4};
        a.change = {0, 0, fs::TimestampSource::readonly_ext4};
    }
    template<Profile P> i64 run(StatCall call, std::span<const u64> args)
    {
        return StatCallAdapter<P>::execute(call, args,
            [&](u64 address, FixedString<256> &path) -> Status {
                order += 'R'; CHECK(address == 0x1000);
                if (!path_status) return path_status;
                for (char ch : name) if (!path.append(ch)) return Status::name_too_long();
                return {};
            },
            [&](StatRequest request, std::string_view path) -> Result<posix::FileStatus> {
                order += 'Q'; queried = request; queried_name = path;
                if (!query_status) return query_status;
                return metadata;
            },
            [&](u64 address, usize length) -> bool {
                order += 'A'; CHECK(address == 0x2000); CHECK(length <= 256);
                return writable;
            },
            [&](u64 address, std::span<const u8> bytes) -> Status {
                order += 'C'; CHECK(address == 0x2000);
                if (!copy_status) return copy_status;
                for (usize i=0;i<bytes.size();++i) output[i+8]=bytes[i];
                return {};
            });
    }
};
template<Profile P> void profile()
{
    const unsigned before = checks;
    using A = StatCallAdapter<P>;
    const std::array<u64,5> basic{0xffffff9cu, 0x1000, 0, 0x7ff, 0x2000};
    for (u32 flags : {0u, 0x100u, 0x800u, 0x1000u, 0x2000u, 0x4000u, 0x3900u, 0x5900u})
    {
        Model m; auto args=basic; args[2]=0xffffffff00000000ull|flags;args[3]=0xffffffff00000000ull|0x40000000u;
        CHECK(m.run<P>(StatCall::statx,args)==0);CHECK(m.order=="RQAC");
        CHECK(m.queried.fd==-100&&m.queried.flags==(flags&0x1100)&&m.queried_name=="/file");
        std::array<u8,256> expected{};CHECK(StatCodec<P>::encode(m.metadata,StatLayout::statx,expected));
        for(usize i=0;i<m.output.size();++i)CHECK(m.output[i]==(i>=8&&i<264?expected[i-8]:0xa5));
    }
    for (u32 flags : {1u,0x6000u,0x8000u,0x80000000u})
    {
        Model m; auto args=basic;args[2]=flags;m.path_status=Status::fault();
        CHECK(m.run<P>(StatCall::statx,args)==-22&&m.order.empty());
    }
    {Model m;auto args=basic;args[3]=0x80000000u;CHECK(m.run<P>(StatCall::statx,args)==-22&&m.order.empty());}
    {Model m;auto args=basic;args[1]=0;CHECK(m.run<P>(StatCall::statx,args)==-14&&m.order.empty());}
    {Model m;m.name="";CHECK(m.run<P>(StatCall::statx,basic)==-2&&m.order=="R");}
    for (u64 address : {0ull,0x1000ull}) for(i32 fd : {-100,-1,7})
    {
        Model m;m.name="";auto args=basic;args[0]=0xfeed00000000ull|static_cast<u32>(fd);args[1]=address;args[2]=0x1900;
        CHECK(m.run<P>(StatCall::statx,args)==0);
        CHECK(m.order==(address?"RQAC":"QAC"));CHECK(m.queried.fd==fd&&m.queried_name.empty());
    }
    for (Status status : {Status::fault(),Status::name_too_long()})
    {
        Model m;m.path_status=status;CHECK(m.run<P>(StatCall::statx,basic)==-syscall::ErrnoMapper::errno_for(status));CHECK(m.order=="R");
    }
    for (Status status : {Status::bad_descriptor(),Status::not_found(),Status::not_directory(),Status::io_error(),Status::unsupported(),Status::invalid_argument(),Status::overflow(),Status::denied(),Status::too_many_symlinks()})
    {
        Model m;m.query_status=status;m.writable=false;const auto old=m.output;
        const i64 expected=status.code()==StatusCode::unsupported?-95:-syscall::ErrnoMapper::errno_for(status);
        CHECK(m.run<P>(StatCall::statx,basic)==expected);CHECK(m.order=="RQ"&&m.output==old);
    }
    {Model m;m.metadata.attributes={};m.writable=false;CHECK(m.run<P>(StatCall::statx,basic)==-95&&m.order=="RQ");}
    {Model m;m.metadata.attributes.device={};m.writable=false;CHECK(m.run<P>(StatCall::statx,basic)==-95&&m.order=="RQ");}
    {Model m;m.metadata.blocks=0x8000000000000000ull;m.writable=false;CHECK(m.run<P>(StatCall::statx,basic)==-75&&m.order=="RQ");}
    {Model m;m.writable=false;const auto old=m.output;CHECK(m.run<P>(StatCall::statx,basic)==-14&&m.order=="RQA"&&m.output==old);}
    {Model m;m.copy_status=Status::fault();const auto old=m.output;CHECK(m.run<P>(StatCall::statx,basic)==-14&&m.order=="RQAC"&&m.output==old);}
    for(u64 address : {u64{0},Codec<P>::word_mask-100})
    {Model m;auto args=basic;args[4]=address;const auto old=m.output;CHECK(m.run<P>(StatCall::statx,args)==-14&&m.order=="RQ"&&m.output==old);}
    {Model m;CHECK(m.run<P>(StatCall::statx,std::span{basic}.first(4))==-14&&m.order.empty());}
    for(auto call : {StatCall::stat,StatCall::lstat,StatCall::fstat,StatCall::newfstatat})
    {
        const bool supported = StatCodec<P>::has_kernel_stat && ((call!=StatCall::stat&&call!=StatCall::lstat)||P==Profile::x86_64);
        const std::array<u64,4> args=call==StatCall::newfstatat?std::array<u64,4>{0xffffff9c,0x1000,0x2000,0x1900}:
            std::array<u64,4>{call==StatCall::fstat?u64{0xffffffff00000007}:u64{0x1000},0x2000,0,0};
        Model m;CHECK(m.run<P>(call,std::span{args}.first(A::argument_count(call)))==(supported?0:-38));
        CHECK(m.order==(supported?(call==StatCall::fstat?"QAC":"RQAC"):""));
        if(supported&&call==StatCall::fstat)CHECK(m.queried.fd==7&&m.queried.direct_fd);
        if(supported&&call==StatCall::lstat)CHECK(m.queried.flags==0x100);
        if(supported&&call==StatCall::newfstatat)
        {Model n;auto bad=args;bad[3]=0x2000;CHECK(n.run<P>(call,bad)==-22&&n.order.empty());}
    }
    {Model m;CHECK(m.run<P>(static_cast<StatCall>(99),basic)==-38&&m.order.empty());}
    std::printf("PROFILE %u checks=%u\n",static_cast<unsigned>(P),checks-before);
}
int main()
{
    profile<Profile::i386>();profile<Profile::x86_64>();profile<Profile::aarch64>();profile<Profile::arm32>();profile<Profile::rv64>();
    profile<Profile::rv32>();profile<Profile::loongarch64>();profile<Profile::mips>();profile<Profile::mips64>();profile<Profile::ppc>();
    std::printf("PASS hosted stat call policy profiles=10 checks=%u\n",checks);
}
