#include <ok/net/tcp_executor.hpp>
using Table = ok::net::tcp::EndpointTable<>;
using Handle = ok::net::tcp::Handle;
using Endpoint = ok::net::tcp::Endpoint;
struct ObjectIss { ok::Result<ok::u32> operator()(Endpoint, Endpoint) { return ok::u32{1}; } };
struct ObjectTransport
{
    ok::Result<ok::net::tcp::ExecutorTransmitState> reap(ok::usize) { return ok::net::tcp::ExecutorTransmitState{}; }
    ok::Result<ok::usize> receive(std::span<std::byte>) { return ok::Status::would_block(); }
    ok::Status try_transmit(ok::net::wire::Bytes) { return ok::Status::would_block(); }
};
extern "C" ok::Result<Handle> tcp_endpoint_listen(Table &t, Endpoint local) { return t.listen(local, 4); }
extern "C" ok::Result<Handle> tcp_endpoint_connect(Table &t, Endpoint a, Endpoint b, ok::u32 iss, ok::u64 now) { return t.connect(a, b, iss, now); }
extern "C" ok::Result<Handle> tcp_endpoint_accept(Table &t, Handle h) { return t.accept(h); }
extern "C" ok::Status tcp_endpoint_release(Table &t, Handle h, ok::u64 now) { return t.release(h, now); }
extern "C" ok::Status tcp_endpoint_retain(Table &t, Handle h) { return t.retain(h); }
extern "C" ok::Result<ok::usize> tcp_endpoint_read(Table &t, Handle h, std::span<std::byte> bytes) { return t.read(h, bytes); }
extern "C" ok::Result<ok::usize> tcp_endpoint_write(Table &t, Handle h, ok::net::wire::Bytes bytes) { return t.write(h, bytes); }
extern "C" ok::Result<ok::net::tcp::Readiness> tcp_endpoint_ready(const Table &t, Handle h) { return t.readiness(h); }
extern "C" ok::Result<ok::net::tcp::IngressResult> tcp_endpoint_ingest(Table &t, ok::net::wire::Bytes bytes, ok::u64 now) { ObjectIss source; return t.ingest(bytes, now, source); }
extern "C" ok::Status tcp_endpoint_executor(Table &t, ok::net::tcp::Executor<Table> &executor, ok::u64 now) { ObjectIss source; ObjectTransport transport; return executor.poll(t, transport, source, now); }
