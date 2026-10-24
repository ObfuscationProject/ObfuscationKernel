#include <ok/user/native_mips64_n64_elf_admission.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace admission = ok::user::mips64_n64_elf_admission;
using admission::Error;
using Bytes = std::vector<std::byte>;
using ok::u64;
static u64 checks;
static void check(bool condition, const char* label)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL %s at check %llu\n", label,
        static_cast<unsigned long long>(checks)); std::exit(1); }
}
static void put(Bytes& b, size_t at, u64 value, size_t width)
{
    for (size_t i = 0; i < width; ++i)
        b.at(at + width - i - 1) = std::byte((value >> (i * 8)) & 255);
}
static void expect(const Bytes& bytes, Error error, const char* label,
                   u64 extent = 0, size_t prefix = std::numeric_limits<size_t>::max(),
                   u64 required = 0)
{
    if (!extent) extent = bytes.size();
    if (prefix == std::numeric_limits<size_t>::max()) prefix = bytes.size();
    auto result = admission::admit(std::span(bytes).first(prefix), extent);
    if (result.error != error || result.required_prefix_bytes != required)
        std::fprintf(stderr, "expected %u/%llu got %u/%llu: ", static_cast<unsigned>(error),
            static_cast<unsigned long long>(required), static_cast<unsigned>(result.error),
            static_cast<unsigned long long>(result.required_prefix_bytes));
    check(result.error == error && result.required_prefix_bytes == required, label);
}
static Bytes fixture()
{
    Bytes b(256);
    put(b,0,0x7f454c46,4); put(b,4,2,1); put(b,5,2,1); put(b,6,1,1);
    put(b,16,2,2); put(b,18,8,2); put(b,20,1,4); put(b,24,0x40000000,8);
    put(b,32,64,8); put(b,48,0x80000005,4); put(b,52,64,2);
    put(b,54,56,2); put(b,56,2,2);
    // One ABI segment followed by PT_NULL; record lies beyond the table.
    put(b,64,0x70000003,4); put(b,72,192,8); put(b,96,24,8); put(b,104,24,8);
    put(b,194,64,1); put(b,195,2,1); put(b,196,2,1); put(b,197,2,1);
    put(b,199,1,1); put(b,208,1,4);
    return b;
}
static void change(size_t offset, u64 value, size_t width, Error error, const char* label)
{
    auto b = fixture(); put(b,offset,value,width); expect(b,error,label);
}
static void declared_fields()
{
    expect(fixture(),Error::none,"synthetic positive");
    for (size_t i=0;i<7;++i) change(i,0,1,Error::invalid_ident,"ELF identity");
    change(4,1,1,Error::invalid_ident,"o32 ELF32");
    change(5,1,1,Error::invalid_ident,"little endian");
    for (unsigned os=0;os<256;++os) for (unsigned version=0;version<256;++version)
    {
        auto b=fixture();put(b,7,os,1);put(b,8,version,1);
        expect(b,((os==0 && version==0)||(os==3 && version==1)) ? Error::none :
               Error::unsupported_osabi,"OSABI version pair");
    }
    change(16,3,2,Error::unsupported_type,"ET_DYN");
    change(16,1,2,Error::unsupported_type,"ET_REL");
    change(18,62,2,Error::unsupported_machine,"wrong machine");
    change(20,0,4,Error::unsupported_version,"ELF version");
    change(52,63,2,Error::unsupported_header_size,"header size");
    for(unsigned flags=0;flags<8;++flags) change(48,0x80000000|flags,4,Error::none,"harmless flags");
    for(unsigned bit=3;bit<31;++bit) change(48,0x80000005|(1U<<bit),4,Error::unsupported_flags,"unsupported eflags bit");
    change(48,5,4,Error::unsupported_flags,"MIPS I");
    change(48,0x70001005,4,Error::unsupported_flags,"o32 ISA32r2");
    change(48,0x80000025,4,Error::unsupported_flags,"n32 ABI2");
    change(48,0x80000405,4,Error::unsupported_flags,"NAN2008");
    change(48,0x82000005,4,Error::unsupported_flags,"microMIPS");
    change(48,0x84000005,4,Error::unsupported_flags,"MIPS16");
    change(48,0x88000005,4,Error::unsupported_flags,"MDMX");
    change(54,55,2,Error::unsupported_program_entry_size,"program entry size");
    change(56,0,2,Error::unsupported_program_count,"no program headers");
    change(56,65535,2,Error::unsupported_program_count,"extended program count");
    change(32,63,8,Error::overlapping_program_table,"program table overlaps header");
    change(64,0,4,Error::missing_abi_flags,"missing record");
    for(auto type:{2U,3U})
    {
        change(64,type,4,Error::dynamic_image,"dynamic first segment");
        auto b=fixture();put(b,120,type,4);expect(b,Error::dynamic_image,"dynamic after ABI",256,176);
    }
    auto b=fixture();put(b,120,0x70000003,4);put(b,128,192,8);put(b,152,24,8);put(b,160,24,8);
    expect(b,Error::duplicate_abi_flags,"duplicate record before prefix refill",256,176);
    for(auto size:{0ULL,23ULL,25ULL,0x100000018ULL})
    {
        change(96,size,8,size>256?Error::segment_outside_file:Error::unsupported_abi_record_size,"record file size");
        change(104,size,8,Error::unsupported_abi_record_size,"record memory size");
    }
    change(192,1,2,Error::unsupported_abi_version,"ABI record version");
    for(unsigned value=0;value<256;++value)
    {
        change(194,value,1,value==64?Error::none:Error::unsupported_isa,"ISA level");
        change(195,value,1,value==2?Error::none:Error::unsupported_isa,"ISA revision");
        change(196,value,1,value==2?Error::none:Error::unsupported_gpr_size,"GPR width");
        change(197,value,1,value==2?Error::none:Error::unsupported_cpr1_size,"CPR1 width");
        change(198,value,1,value==0?Error::none:Error::unsupported_cpr2_size,"CPR2 width");
        change(199,value,1,value==1?Error::none:Error::unsupported_fp_abi,"FP ABI including soft float");
    }
    for(unsigned bit=0;bit<32;++bit)
    {
        change(200,u64{1}<<bit,4,Error::unsupported_isa_extension,"ISA extensions");
        change(204,u64{1}<<bit,4,Error::unsupported_ases,"ASE including DSP MSA MDMX");
        change(208,u64{1}<<bit,4,bit==0?Error::none:Error::unsupported_abi_flags1,"flags1");
        change(212,u64{1}<<bit,4,Error::unsupported_abi_flags2,"flags2");
    }
    change(208,0,4,Error::none,"ODDSPREG optional");
}
static void boundaries()
{
    auto b=fixture();
    check(admission::admit({},0).error==Error::header_outside_file,"empty file");
    for(size_t n=0;n<64;++n)
    {
        expect(b,Error::header_outside_prefix,"short header prefix",256,n,64);
        check(admission::admit(std::span(b).first(n),n).error==Error::header_outside_file,"short complete header");
    }
    expect(b,Error::prefix_exceeds_file,"prefix larger than file",255);
    for(size_t n=64;n<176;++n) expect(b,Error::program_table_outside_prefix,"short program prefix",256,n,176);
    for(size_t n=176;n<216;++n) expect(b,Error::abi_record_outside_prefix,"short record prefix",256,n,216);
    for(size_t n=216;n<=256;++n) expect(b,Error::none,"sufficient prefix",256,n);
    expect(b,Error::program_table_outside_file,"truncated program table",175,64);
    expect(b,Error::segment_outside_file,"truncated ABI payload",215,176);
    change(72,257,8,Error::segment_outside_file,"offset past file");
    change(72,0xffffffffffffffffULL,8,Error::segment_outside_file,"offset addition overflow");
    change(96,0xffffffffffffffffULL,8,Error::segment_outside_file,"size addition overflow");
    change(32,0x100000040ULL,8,Error::program_table_outside_file,"table high bits retained");
    change(72,0x1000000c0ULL,8,Error::segment_outside_file,"segment high bits retained");
    constexpr auto maximum=std::numeric_limits<u64>::max();
    for(u64 table:{0x100000040ULL,0x8000000000000040ULL})
    {
        b=fixture();put(b,32,table,8);
        expect(b,Error::program_table_outside_prefix,"large checked table prefix",maximum,256,table+112);
    }
    b=fixture();put(b,32,maximum-111,8);
    expect(b,Error::program_table_outside_file,"table range wraps",maximum);
    put(b,32,maximum-112,8);
    expect(b,Error::program_table_outside_prefix,"table ends exactly at UINT64_MAX",maximum,256,maximum);
    for(u64 record:std::array<u64,3>{0x1000000c0ULL,0x80000000000000c0ULL,maximum-24})
    {
        b=fixture();put(b,72,record,8);
        expect(b,Error::abi_record_outside_prefix,"large checked ABI prefix",maximum,256,record+24);
    }
    b=fixture();put(b,72,maximum-23,8);
    expect(b,Error::segment_outside_file,"ABI range wraps",maximum);
    // Non-ABI segments are bounded too; undefined PT_NULL payloads are ignored.
    b=fixture();put(b,128,maximum,8);put(b,152,maximum,8);
    expect(b,Error::none,"undefined PT_NULL payload");
    put(b,120,1,4);expect(b,Error::segment_outside_file,"LOAD payload overflow");
    put(b,128,256,8);put(b,152,0,8);expect(b,Error::none,"empty segment at EOF");
    put(b,128,257,8);expect(b,Error::segment_outside_file,"empty segment past EOF");
    b=fixture();put(b,56,65534,2);expect(b,Error::program_table_outside_prefix,"maximum bounded count",maximum,256,64+65534ULL*56);
}
static Bytes read(const std::string& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);check(bool(file),"open exact ELF");
    auto n=file.tellg();check(n>0,"ELF size");Bytes b(static_cast<size_t>(n));file.seekg(0);
    file.read(reinterpret_cast<char*>(b.data()),n);check(bool(file),"read exact ELF");return b;
}
static void actual(int argc, char **argv)
{
    for(int i=1;i<argc;++i)
    {
        const char *name=argv[i];
        auto b=read(name);expect(b,Error::none,name);
        size_t prefix=0;unsigned steps=0;
        for(;;)
        {
            auto r=admission::admit(std::span(b).first(prefix),b.size());
            if(r)break;
            check(r.required_prefix_bytes>prefix && r.required_prefix_bytes<=b.size(),"bounded incremental actual prefix");
            prefix=static_cast<size_t>(r.required_prefix_bytes);check(++steps<=3,"at most three actual prefix refills");
        }
        const auto full=admission::admit(b,b.size());check(full.ok(),"full actual repeat");
        std::printf("ACTUAL_PASS %s bytes=%zu prefix=%zu refills=%u\n",name,b.size(),prefix,steps);
        for(unsigned flags=0;flags<8;++flags)
        {
            auto changed=b;put(changed,48,0x80000000|flags,4);expect(changed,Error::none,"actual harmless eflags variant");
        }
    }
}
static void fuzz_bounds()
{
    // Deterministic malformed-byte exploration checks memory safety and prefix
    // contracts only; it is not an independent oracle for semantic acceptance.
    u64 state=0xa941be675019fd23ULL;
    const auto next=[&]() {state^=state<<13;state^=state>>7;state^=state<<17;return state;};
    for(unsigned iteration=0;iteration<30000;++iteration)
    {
        auto b=fixture();
        for(unsigned changes=1+next()%5;changes;--changes)put(b,next()%256,next()&255,1);
        size_t prefix=next()%257;u64 extent=(iteration%3==0)?next():next()%1024;
        auto r=admission::admit(std::span(b).first(prefix),extent);
        bool shortage=r.error==Error::header_outside_prefix || r.error==Error::program_table_outside_prefix || r.error==Error::abi_record_outside_prefix;
        check(shortage?(r.required_prefix_bytes>prefix && r.required_prefix_bytes<=extent):r.required_prefix_bytes==0,"fuzz prefix contract");
        if(r)check(prefix>=64 && prefix<=extent,"fuzz accepted prefix bound");
    }
}
int main(int argc,char**argv)
{
    declared_fields();boundaries();actual(argc,argv);fuzz_bounds();
    std::printf("N64_ELF_MODEL_PASS checks=%llu fuzz_iterations=30000\n",static_cast<unsigned long long>(checks));
}
