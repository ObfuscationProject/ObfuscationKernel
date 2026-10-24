#include "ok/abi/linux_udp_call.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
using namespace ok::abi::linux_user;
namespace
{
usize checks{};
void check(bool value, const char *message)
{
    ++checks;
    if (!value) { std::fprintf(stderr, "Linux UDP raw model failure: %s\n", message); std::abort(); }
}
template <typename T> void error(const Result<T> &value, StatusCode code, const char *message)
{
    check(!value && value.status().code() == code, message);
}
struct Copy final
{
    std::array<u8, 48> source{};
    usize available{}, calls{}, copied{};
    u64 expected_address{0x1000}, observed_address{};
    bool fail{};
    Call *mutate{};
    Copy() = default;
    Copy(const Copy &) = delete;
    Copy &operator=(const Copy &) = delete;
    Status operator()(u64 address, std::span<u8> output)
    {
        ++calls; observed_address = address; copied = output.size();
        if (address != expected_address || output.size() > available || fail)
            return Status::fault("model copy authority refused the interval");
        for (usize index = 0; index < output.size(); ++index) output[index] = source[index];
        if (mutate) *mutate = {};
        return Status::success();
    }
};
struct WrongCopy { bool operator()(u64, std::span<u8>); };
static_assert(UdpArgumentCopy<Copy> && !UdpArgumentCopy<WrongCopy>);

constexpr std::array<std::array<u32, 5>, 10> golden_numbers{{
    {359,361,369,371,102}, {41,49,44,45,unavailable_number},
    {198,200,206,207,unavailable_number}, {281,282,290,292,unavailable_number},
    {198,200,206,207,unavailable_number}, {198,200,206,207,unavailable_number},
    {198,200,206,207,unavailable_number}, {4183,4169,4180,4176,4102},
    {5040,5048,5043,5044,unavailable_number}, {326,327,335,337,102}}};

template <Profile P> void put(std::span<u8> bytes, u64 value)
{
    for (usize index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<u8>(value >> (Codec<P>::profile.big_endian ? (bytes.size()-index-1)*8 : index*8));
}
template <Profile P> RegisterFile frame(const Call &raw, std::span<u8> stack)
{
    RegisterFile result{};
    result.integer[Codec<P>::profile.number_register] = raw.number;
    for (usize index = 0; index < raw.argument_count && index < 6; ++index)
    {
        const i8 reg = Codec<P>::profile.argument_registers[index];
        if (reg >= 0) result.integer[static_cast<usize>(reg)] = raw.arguments[index];
        else put<P>(stack.subspan(index*4,4),raw.arguments[index]);
    }
    return result;
}

template <Profile P> void run()
{
    using C = UdpConstants<P>;
    using D = UdpCallDecoder<P>;
    using W = UdpWire<P>;
    constexpr bool mips = P == Profile::mips || P == Profile::mips64;
    constexpr u64 high = Codec<P>::profile.word_bytes == 8 ? 0xa5a5a5a500000000ULL : 0;
    constexpr u64 mask = Codec<P>::word_mask;
    const auto &numbers = golden_numbers[static_cast<usize>(P)];
    for (usize index = 0; index < 4; ++index)
        check(C::syscall_number(static_cast<UdpCall>(index)) == numbers[index], "literal direct syscall golden");
    check(C::socketcall_number().value_or(unavailable_number) == numbers[4], "literal socketcall availability golden");
    check(!C::syscall_number(static_cast<UdpCall>(255)), "invalid call enum");
    check(C::sock_dgram == (mips ? 1 : 2) && C::sock_stream == (mips ? 2 : 1), "MIPS type reversal");
    check(C::sock_nonblock == (mips ? 128 : 2048) && C::sock_cloexec == 524288, "socket flag goldens");
    check(C::sock_type_mask == 15 && C::af_inet == 2 && C::af_unspec == 0 && C::protocol_udp == 17, "family protocol mask goldens");
    check(C::message(UdpMessage::dontwait) == 64 && C::message(UdpMessage::peek) == 2 && C::message(UdpMessage::trunc) == 32, "receive flag goldens");
    check(C::message(UdpMessage::nosignal) == 16384 && C::message(UdpMessage::cmsg_cloexec) == 1073741824, "large flags stay unsigned32");
    check(!C::message(static_cast<UdpMessage>(255)) && !C::errno_number(static_cast<UdpErrno>(255)), "invalid constant enum");
    check(C::errno_number(UdpErrno::e_opnotsupp) == (mips ? 122 : 95), "architecture EOPNOTSUPP golden");
    check(C::errno_number(UdpErrno::e_nosys) == (mips ? 89 : 38), "architecture ENOSYS golden");
    check(C::errno_number(UdpErrno::e_overflow) == (mips ? 79 : 75), "architecture EOVERFLOW golden");
    check(C::errno_number(UdpErrno::e_afnosupport) == (mips ? 124 : 97), "architecture EAFNOSUPPORT golden");
    check(C::decode_error_number(StatusCode::not_found) == C::errno_number(UdpErrno::e_nosys), "unsupported number differs from supported selector");
    check(C::decode_error_number(StatusCode::unsupported) == C::errno_number(UdpErrno::e_opnotsupp), "out of slice selector errno");
    check(!C::decode_error_number(StatusCode::busy), "arbitrary authority status not translated");
    check(C::send_flags_mask() == 0x4040 && C::receive_flags_mask() == 0x62, "explicit supported slice masks");

    Call socket{numbers[0], {high|2, high|C::sock_dgram|C::sock_nonblock|C::sock_cloexec, high|17, ~u64{}, ~u64{}, ~u64{}}, 6};
    auto decoded = D::decode_direct(socket);
    check(decoded && decoded.value() == UdpRequest{.call=UdpCall::socket, .domain=2,
        .type=static_cast<i32>(C::sock_dgram|C::sock_nonblock|C::sock_cloexec), .protocol=17}, "socket low32 and unused words");
    socket.argument_count=2; error(D::decode_direct(socket),StatusCode::invalid_argument,"short socket snapshot");
    socket.argument_count=7; error(D::decode_direct(socket),StatusCode::invalid_argument,"oversized snapshot");
    socket.argument_count=3;
    std::array<u8,24> stack{}; auto registers=frame<P>(socket,stack);
    check(D::decode_direct(registers).value() == decoded.value(), "socket register normalization");
    registers.integer[Codec<P>::profile.number_register]=0x100000001ULL;
    error(D::decode_direct(registers),StatusCode::overflow,"noncanonical number snapshot");
    if constexpr (Codec<P>::profile.word_bytes == 4)
    {
        socket.arguments[0]=0x100000002ULL;
        error(D::decode_direct(socket),StatusCode::overflow,"noncanonical target word before int narrowing");
        socket.arguments[0]=2;
    }

    Call bind{numbers[1], {high|0xffffffffU, mask, high|0x80000001U}, 3};
    auto bound=D::decode_direct(bind);
    check(bound && bound.value() == UdpRequest{.call=UdpCall::bind,.fd=-1,.address=mask,.address_length=-2147483647}, "bind signed32 address length");
    check(D::decode_direct(frame<P>(bind,stack)).value()==bound.value(),"bind register words");
    Call send{numbers[2], {high|0xffffffffU,mask,mask,high|0xdeadbeefU,0,high|0xfffffff0U},6};
    auto sent=D::decode_direct(send);
    check(sent && sent.value()==UdpRequest{.call=UdpCall::sendto,.fd=-1,.payload=mask,.count=mask,
        .flags=0xdeadbeefU,.address=0,.address_length=-16},"send target pointers size and unsigned32 flags");
    auto send_frame=frame<P>(send,stack);
    check(D::decode_direct(send_frame,stack).value()==sent.value(),"six argument register/stack normalization");
    if constexpr (P == Profile::mips)
    {
        error(D::decode_direct(send_frame),StatusCode::fault,"MIPS O32 stack copy required");
        error(D::decode_direct(send_frame,std::span<const u8>{stack}.first(20)),StatusCode::fault,"MIPS O32 sixth slot boundary");
        stack[0]=0xff;stack[15]=0xff;
        check(D::decode_direct(send_frame,stack).value()==sent.value(),"MIPS home slots ignored");
    }
    Call receive=send;receive.number=numbers[3];receive.arguments[5]=mask;
    auto received=D::decode_direct(receive);
    check(received && received.value()==UdpRequest{.call=UdpCall::recvfrom,.fd=-1,.payload=mask,.count=mask,
        .flags=0xdeadbeefU,.address=0,.source_length=mask},"recv source length is pointer not signed int");
    check(D::decode_direct(frame<P>(receive,stack),stack).value()==received.value(),"recv target slot six");
    error(D::decode_direct(Call{unavailable_number,{},0}),StatusCode::not_found,"unrelated number rejected");

    Copy copy; Call multiplex{numbers[4], {1,0x1000}, 2};
    if (numbers[4] == unavailable_number)
    {
        error(D::decode_socketcall(multiplex,copy),StatusCode::not_found,"socketcall absent profile");
        error(D::decode_socketcall(frame<P>(multiplex,stack),copy),StatusCode::not_found,"socketcall absent register profile");
        check(copy.calls==0,"absent socketcall cannot invoke authority");
    }
    else
    {
        std::array<Call,4> originals{socket,bind,send,receive};
        for (usize index=0;index<4;++index)
        {
            auto raw=originals[index]; raw.argument_count=index<2?3:6;
            const usize bytes=raw.argument_count*Codec<P>::profile.word_bytes;
            copy.available=bytes;
            for(usize at=0;at<raw.argument_count;++at)
                put<P>(std::span<u8>{copy.source}.subspan(at*Codec<P>::profile.word_bytes,Codec<P>::profile.word_bytes),raw.arguments[at]);
            multiplex={numbers[4],{udp_subcalls[index],0x1000},2};copy.mutate=&multiplex;
            const usize before=copy.calls;
            const auto result=D::decode_socketcall(multiplex,copy);
            check(result && result.value()==D::decode_direct(raw).value(),"socketcall/direct parity and input mutation");
            check(copy.calls==before+1 && copy.copied==bytes && copy.observed_address==0x1000,"one exact authority call");
            copy.mutate=nullptr;
            multiplex={numbers[4],{udp_subcalls[index],0x1000},2};
            check(D::decode_socketcall(frame<P>(multiplex,stack),copy).value()==result.value(),"socketcall register parity");
            for(usize missing=0;missing<bytes;++missing)
            {
                copy.available=missing;
                error(D::decode_socketcall(multiplex,copy),StatusCode::fault,"each copied array byte required");
            }
        }
        const usize before=copy.calls;
        for(u32 selector: {0U,21U,0xffffffffU})
        { multiplex.arguments[0]=selector;error(D::decode_socketcall(multiplex,copy),StatusCode::invalid_argument,"invalid selector before copy"); }
        multiplex.arguments[0]=4;error(D::decode_socketcall(multiplex,copy),StatusCode::unsupported,"valid out of slice selector");
        check(copy.calls==before,"selector refusal has no copy");
        multiplex.arguments={1,mask};error(D::decode_socketcall(multiplex,copy),StatusCode::fault,"target address interval wrap");
        check(copy.calls==before,"wrap has no copy");
        multiplex.arguments={1,0x1000};multiplex.argument_count=1;
        error(D::decode_socketcall(multiplex,copy),StatusCode::invalid_argument,"short outer arguments");
        multiplex.argument_count=2;multiplex.arguments[1]=0x100001000ULL;
        error(D::decode_socketcall(multiplex,copy),StatusCode::overflow,"outer pointer target word overflow");
    }

    std::array<u8,129> address{};address.fill(0xa5);
    const std::array<u8,16> little{2,0,0x9c,0x40,192,0,2,1,0,0,0,0,0,0,0,0};
    const std::array<u8,16> big{0,2,0x9c,0x40,192,0,2,1,0,0,0,0,0,0,0,0};
    const auto &golden=Codec<P>::profile.big_endian?big:little;
    for(usize index=0;index<8;++index)address[index]=golden[index];
    for(usize length:{usize{16},usize{17},usize{128}})
    {
        auto value=W::decode_address(std::span<const u8>{address}.first(length));
        check(value && value.value().family==2 && value.value().port==40000 && value.value().address==net::wire::Ipv4{192,0,2,1},"copied address padding and tail ignored");
    }
    error(W::decode_address(std::span<const u8>{address}.first(15)),StatusCode::invalid_argument,"short copied IPv4");
    error(W::decode_address(address),StatusCode::invalid_argument,"overlong copied IPv4");
    address[0]=0xfe;address[1]=0xff;
    check(W::decode_address(std::span<const u8>{address}.first(16)).value().family==(Codec<P>::profile.big_endian?0xfeff:0xfffe),"raw family preserved without admission");
    std::array<u8,16> output{};output.fill(0xaa);
    check(W::encode_address(output,{2,40000,{192,0,2,1}}).ok() && output==golden,"literal target address output");
    const auto saved=output;
    check(!W::encode_address(std::span<u8>{output}.first(15),{}) && output==saved,"wrong address output size atomic");
    std::array<u8,4> length{};
    check(W::encode_socklen(length,16).ok(),"encode socklen16");
    check(length==(Codec<P>::profile.big_endian?std::array<u8,4>{0,0,0,16}:std::array<u8,4>{16,0,0,0}),"literal four-byte target socklen");
    check(W::decode_socklen(length).value()==16,"decode signed capacity16");
    length.fill(0xff);check(W::decode_socklen(length).value()==-1,"negative capacity preserved");
    const auto length_saved=length;
    check(!W::encode_socklen(length,0x80000000U) && length==length_saved,"actual socklen overflow atomic");
    check(!W::encode_socklen(std::span<u8>{length}.first(3),16) && length==length_saved,"short socklen output atomic");
    error(W::decode_socklen(std::span<const u8>{length}.first(3)),StatusCode::invalid_argument,"short socklen input");
    check(W::encode_socklen(length,0x7fffffffU).ok() && W::decode_socklen(length).value()==2147483647,"signed capacity boundary");
}
}

int main()
{
    run<Profile::i386>();run<Profile::x86_64>();run<Profile::aarch64>();run<Profile::arm32>();
    run<Profile::rv64>();run<Profile::rv32>();run<Profile::loongarch64>();
    run<Profile::mips>();run<Profile::mips64>();run<Profile::ppc>();
    std::printf("LINUX_UDP_RAW_MODEL_PASS profiles=10 checks=%zu\n",checks);
}
