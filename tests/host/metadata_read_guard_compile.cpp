#include <ok/fs/ext4/txn.hpp>

using Cache = ok::fs::MetadataBuffers<2, 1024, 2>;
struct Guard
{
    ok::Status begin_read(Cache::Registry::Session &session, ok::u64, ok::usize bytes)
    { if (bytes != 1024) return ok::Status::unsupported("guarded transfer size");
  const auto report = session.report(); return report ? ok::Status{} : report.status(); }
    ok::Status finish_read(Cache::Registry::Session &session, ok::Status status)
    { const auto report = session.report(); return status ? (report ? ok::Status{} : report.status()) : status; }
};
static_assert(ok::fs::MetadataReadDispatchGuard<Guard, Cache::Registry::Session>);
ok::Status qualify_metadata_read_guard(Cache &cache, Cache::Pin &pin, Guard &guard)
{ return cache.pin(1, pin, guard); }
