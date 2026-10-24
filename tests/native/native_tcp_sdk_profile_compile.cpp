#include <ok/net/native_tcp_sdk_profile.hpp>

ok::usize native_tcp_sdk_profile_compile()
{
    return ok::net::native_tcp_stream_capacity + ok::net::native_tcp_poll_budget;
}
