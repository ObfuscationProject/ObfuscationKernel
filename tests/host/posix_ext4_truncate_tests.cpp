#include "ext4_realtime_model.hpp"
// Hosted common POSIX + real ext4 replay. No Virtio transport or guest CPU.
#include "ok/fs/ext4/layout.hpp"
#include "ok/posix/posix.hpp"
#include "ok/syscall/linux.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace ok;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                          \
      std::abort();                                                            \
    }                                                                          \
  } while (false)
using Bytes = std::vector<std::byte>;
struct Event {
  char kind;
  u64 first;
  usize bytes;
};
class Disk final : public driver::BlockDevice {
public:
  explicit Disk(const Bytes &image) : bytes(image) {}
  driver::BlockGeometry geometry() const override {
    return {bytes.size() / 512, 512, true};
  }
  bool fails(char kind, u64 first, usize length) {
    if (!armed)
      return false;
    trace.push_back({kind, first, length});
    if (trace.size() == fail_at || (persistent && trace.size() > fail_at)) {
      triggered = true;
      return true;
    }
    return false;
  }
  Status read_blocks(u64 first, std::span<std::byte> out) override {
    CHECK(first * 512 + out.size() <= bytes.size());
    if (fails('r', first, out.size()))
      return Status::io_error("injected read failure");
    std::memcpy(out.data(), bytes.data() + first * 512, out.size());
    return Status::success();
  }
  Status write_blocks(u64 first, std::span<const std::byte> in) override {
    CHECK(first * 512 + in.size() <= bytes.size());
    if (fails('w', first, in.size()))
      return Status::io_error("injected write failure");
    std::memcpy(bytes.data() + first * 512, in.data(), in.size());
    return Status::success();
  }
  Status flush() override {
    return fails('f', 0, 0) ? Status::io_error("injected flush failure")
                            : Status::success();
  }
  void arm(usize failure = ~usize{0}, bool repeat = false) {
    armed = true;
    triggered = false;
    fail_at = failure;
    persistent = repeat;
    trace.clear();
  }
  void disarm() { armed = false; }
  Bytes bytes;
  std::vector<Event> trace;
  bool armed{}, triggered{}, persistent{};
  usize fail_at{};
};
std::span<const std::byte> bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte *>(s.data()), s.size()};
}
struct Fixture {
  Disk disk;
  fs::Ext4FileSystem fs;
  sched::Scheduler scheduler;
  fs::VirtualFileSystem vfs;
  driver::ConsoleDriver console;
  posix::PosixService service;
  posix::Fd file{};
  explicit Fixture(const Bytes &image) : disk(image) {
    CHECK(fs.mount(disk));
    CHECK(vfs.mount("/", *fs.root()));
    CHECK(console.start());
    CHECK(service.initialize(vfs, console, scheduler));
    auto pid = scheduler.create_user_process(
        "truncate-replay", arch::configured_arch_operations().make_user_context(
                               {0x400000, 0x800000, 0}));
    CHECK(pid);
    CHECK(service.activate(pid.value()));
    CHECK(service.set_identity(0, 0));
    CHECK(vfs.create("/tmp", fs::NodeType::directory));
    auto opened = service.open(
        "/tmp/posix-io", posix::o_CREAT | posix::o_RDWR | posix::o_TRUNC, 0600);
    CHECK(opened);
    file = opened.value();
    CHECK(service.write(file, bytes("abcdef")).value() == 6);
    CHECK(service.seek(file, 0, posix::SeekWhence::set));
    CHECK(service.write(file, bytes("X")).value() == 1);
    CHECK(service.pwrite(file, bytes("Y"), 2).value() == 1);
    CHECK(service.ftruncate(file, 3));
    CHECK(service.ftruncate(file, 12));
    std::array<std::byte, 16> read{};
    CHECK(service.pread(file, read, 3).value() == 9);
    for (usize i = 0; i < 9; ++i)
      CHECK(read[i] == std::byte{});
    CHECK(service.seek(file, 0, posix::SeekWhence::set));
    const posix::IoVector out[]{{reinterpret_cast<uptr>("vec"), 3},
                                {reinterpret_cast<uptr>("tor"), 3}};
    CHECK(service.writev(file, out).value() == 6);
    CHECK(service.fsync(file));
    CHECK(service.fsync(file));
    CHECK(scheduler.open_description_count() == 4 &&
          service.open_file_count() == 4);
  }
  ~Fixture() {
    disk.disarm();
    CHECK(service.close(file));
  }
};

void open_cleanup_sweep(const Bytes &baseline) {
  for (const u32 flags : {posix::o_RDONLY, posix::o_RDWR | posix::o_TRUNC}) {
    auto *clean = new Fixture(baseline);
    clean->disk.arm();
    const auto opened = clean->service.open("/tmp/posix-io", flags);
    CHECK(opened && opened.value() == 4);
    const auto trace = clean->disk.trace;
    clean->disk.disarm();
    CHECK(clean->service.close(opened.value()));
    delete clean;
    for (usize failure = 1; failure <= trace.size(); ++failure)
      for (bool persistent : {false, true}) {
        auto *f = new Fixture(baseline);
        f->disk.arm(failure, persistent);
        const auto result = f->service.open("/tmp/posix-io", flags);
        CHECK(f->disk.triggered);
        f->disk.disarm();
        CHECK(!result && result.status().code() == StatusCode::io_error);
        CHECK(f->scheduler.open_description_count() == 4 &&
              f->service.open_file_count() == 4);
        CHECK(!f->fs.sync());
        std::array<std::byte, 8> buffer{};
        CHECK(f->service.pread(f->file, buffer, 0));
        const auto retry = f->service.open("/tmp/posix-io", posix::o_RDONLY);
        CHECK(retry && retry.value() == 4);
        CHECK(f->service.close(retry.value()));
        CHECK(f->scheduler.open_description_count() == 4 &&
              f->service.open_file_count() == 4);
        delete f;
      }
    std::printf("PASS open flags=%u: %zu single and %zu persistent I/O faults "
                "report EIO; old fd valid, new fd reused\n",
                flags, trace.size(), trace.size());
  }
  // Creation may have visible filesystem effects before final metadata
  // inspection reports an error. Neither final metadata read may publish a fd.
  // Counted path release itself no longer reads the inode.
  auto *clean = new Fixture(baseline);
  clean->disk.arm();
  const auto opened =
      clean->service.open("/tmp/new", posix::o_CREAT | posix::o_RDWR, 0600);
  CHECK(opened);
  const auto trace = clean->disk.trace;
  clean->disk.disarm();
  CHECK(clean->service.close(opened.value()));
  delete clean;
  CHECK(trace.size() >= 2 && trace[trace.size() - 2].kind == 'r' &&
        trace.back().kind == 'r');
  for (usize failure = trace.size() - 1; failure <= trace.size(); ++failure) {
    auto *f = new Fixture(baseline);
    f->disk.arm(failure, true);
    const auto result =
        f->service.open("/tmp/new", posix::o_CREAT | posix::o_RDWR, 0600);
    CHECK(f->disk.triggered);
    f->disk.disarm();
    CHECK(!result && result.status().code() == StatusCode::io_error);
    CHECK(f->scheduler.open_description_count() == 4 &&
          f->service.open_file_count() == 4);
    CHECK(!f->fs.sync());
    const auto retry = f->service.open("/tmp/new", posix::o_RDONLY);
    CHECK(retry && retry.value() == 4);
    CHECK(f->service.close(retry.value()));
    delete f;
  }
  std::puts("PASS new-file open: both final metadata reads fail before fd "
            "publication, persistent rollback, fd reused");
}

int main(int argc, char **argv) {
    testing::ext4_clock::reset();
    struct LegacyClockGuard { ~LegacyClockGuard() { CHECK(testing::ext4_clock::calls() == 0); } } legacy_clock_guard;
  CHECK(argc == 2);
  auto *file = std::fopen(argv[1], "rb");
  CHECK(file);
  CHECK(!std::fseek(file, 0, SEEK_END));
  const auto size = std::ftell(file);
  CHECK(size > 0);
  std::rewind(file);
  Bytes baseline(static_cast<usize>(size));
  CHECK(std::fread(baseline.data(), 1, baseline.size(), file) ==
        baseline.size());
  std::fclose(file);
  open_cleanup_sweep(baseline);
  auto *success = new Fixture(baseline);
  success->disk.arm();
  CHECK(success->service.truncate("/tmp/posix-io", 2));
  const auto trace = success->disk.trace;
  success->disk.disarm();
  CHECK(success->service.fstat(success->file).value().size == 2);
  CHECK(success->scheduler.open_description_count() == 4 &&
        success->service.open_file_count() == 4);
  for (usize i = 0; i < trace.size(); ++i)
    std::printf("TRACE %zu %c sector=%llu bytes=%zu\n", i + 1, trace[i].kind,
                static_cast<unsigned long long>(trace[i].first),
                trace[i].bytes);
  for (usize attempt = 0; attempt < 100; ++attempt) {
    CHECK(success->service.ftruncate(success->file, 12));
    CHECK(success->service.fsync(success->file));
    CHECK(success->service.fsync(success->file));
    CHECK(success->service.truncate("/tmp/posix-io", 2));
    CHECK(success->service.fstat(success->file).value().size == 2);
    CHECK(success->scheduler.open_description_count() == 4 &&
          success->service.open_file_count() == 4);
  }
  CHECK(success->service.fsync(success->file));
  delete success;
  std::printf("PASS 101 clean sync/sync/path-truncate replays, no descriptor "
              "growth; %zu I/O boundaries\n",
              trace.size());
  usize hidden = 0;
  for (usize failure = 1; failure <= trace.size(); ++failure) {
    auto *f = new Fixture(baseline);
    f->disk.arm(failure);
    const auto status = f->service.truncate("/tmp/posix-io", 2);
    CHECK(f->disk.triggered);
    f->disk.disarm();
    CHECK(f->scheduler.open_description_count() == 4 &&
          f->service.open_file_count() == 4);
    CHECK(!f->fs.sync());
    std::printf("INJECT %zu %c sector=%llu => status=%u errno=%lld text=%.*s\n",
                failure, trace[failure - 1].kind,
                static_cast<unsigned long long>(trace[failure - 1].first),
                static_cast<unsigned>(status.code()),
                static_cast<long long>(syscall::ErrnoMapper::errno_for(status)),
                static_cast<int>(status.message().size()),
                status.message().data());
    CHECK(status.code() == StatusCode::io_error);
    hidden += status.ok();
    delete f;
  }
  CHECK(hidden == 0);
  std::printf("PASS %zu injected failures: temporary descriptions all "
              "released; %zu late/cleanup failures hidden by "
              "successful truncate result\n",
              trace.size(), hidden);

  // Fail the final inode write persistently. The primary error must win;
  // closing a linked temporary description consumes its hold without I/O.
  usize last_write = 0;
  for (usize i = 0; i < trace.size(); ++i)
    if (trace[i].kind == 'w')
      last_write = i + 1;
  CHECK(last_write != 0 && last_write == trace.size());
  auto *dual = new Fixture(baseline);
  dual->disk.arm(last_write, true);
  const auto primary = dual->service.truncate("/tmp/posix-io", 2);
  CHECK(primary.code() == StatusCode::io_error);
  CHECK(primary.message() == "injected write failure");
  CHECK(dual->disk.trace.size() == last_write &&
        dual->disk.trace.back().kind == 'w');
  CHECK(dual->scheduler.open_description_count() == 4 &&
        dual->service.open_file_count() == 4);
  CHECK(!dual->fs.sync());
  delete dual;
  std::puts(
      "PASS primary truncate error preserved; temporary close consumes hold without I/O");
}
