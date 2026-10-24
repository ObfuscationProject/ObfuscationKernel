// Saved-register model only; no target instructions or guest memory execute.
#include "ok/abi/linux_chmod_call.hpp"
#include "ok/abi/linux_native.hpp"
#include <cstdio>
#include <cstdlib>
using namespace ok;
using namespace ok::abi::linux_user;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%d: %s\n",__LINE__,#x); std::abort(); } } while(false)

int main()
{
    using Bridge = NativeBridge<Profile::mips>;
    using Adapter = ChmodCallAdapter<Profile::mips>;
    for (uptr sp : {uptr{0}, uptr{0xfffffff0}, ~uptr{0}})
    {
        Bridge::Frame frame{};
        frame.sp = sp;
        frame.registers[4] = 0xfeed0000ffffff9cull;
        frame.registers[5] = 0xfeed000012345678ull;
        frame.registers[6] = ~uptr{0};
        frame.registers[7] = 0x1100;
        for (auto call : {ChmodCall::chmod, ChmodCall::fchmod, ChmodCall::fchmodat, ChmodCall::fchmodat2})
        {
            const auto operation = call == ChmodCall::chmod ? Operation::chmod :
                call == ChmodCall::fchmod ? Operation::fchmod :
                call == ChmodCall::fchmodat ? Operation::fchmodat : Operation::fchmodat2;
            frame.registers[2] = *Codec<Profile::mips>::syscall_number(operation);
            unsigned copies = 0;
            auto captured = Bridge::decode_call_copying_stack(frame, Adapter::argument_count(call),
                [&](uptr, std::span<u8>) -> Status { ++copies; return Status::fault(); });
            CHECK(captured && copies == 0);
            const auto &arguments = captured.value().arguments;
            auto decoded = Adapter::decode(call, std::span{arguments}.first(captured.value().argument_count));
            CHECK(decoded);
            if (call == ChmodCall::fchmodat || call == ChmodCall::fchmodat2)
            {
                CHECK(decoded.value().fd == -100 && decoded.value().path == 0x12345678);
                CHECK(decoded.value().permissions == 07777);
                CHECK(decoded.value().flags == (call == ChmodCall::fchmodat2 ? 0x1100u : 0u));
            }
        }
    }
    std::printf("PASS chmod MIPS O32 register-only bridge checks=%u guest=0\n",checks);
}
