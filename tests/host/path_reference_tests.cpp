// Hosted service-boundary ownership model; no storage transport or guest CPU.
#include "ok/posix/posix.hpp"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

using namespace ok;
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%d: %s\n", __LINE__, #condition);                  \
      std::abort();                                                            \
    }                                                                          \
  } while (false)

class CountedNode final : public fs::Node {
public:
  fs::NodeType type{fs::NodeType::regular};
  CountedNode *child{};
  posix::PosixService *service{};
  usize pins{}, retain_calls{}, release_calls{}, truncate_calls{};
  mutable usize queries{};
  usize fail_retain{}, fail_release{}, fail_query{};
  bool fail_all_releases{}, fail_truncate{};
  usize first_release_fds{};
  std::string_view name() const override { return "counted"; }
  fs::Metadata metadata() const override {
    return {.type = type, .size = 6, .mode = fs::mode_for(type, 0777)};
  }
  Result<fs::Metadata> query_metadata() const override {
    if (++queries == fail_query)
      return Status::io_error("primary metadata failure");
    return metadata();
  }
  Result<fs::FileBuffer> read(usize, usize) const override {
    return fs::FileBuffer{};
  }
  Status write(usize, std::span<const std::byte>) override {
    return Status::success();
  }
  Status truncate(usize) override {
    ++truncate_calls;
    return fail_truncate ? Status::no_space("primary truncate failure")
                         : Status::success();
  }
  Status retain() override {
    if (++retain_calls == fail_retain)
      return Status::no_memory("primary retain failure");
    ++pins;
    return Status::success();
  }
  Status release() override {
    CHECK(pins != 0);
    --pins;
    ++release_calls;
    if (release_calls == 1 && service)
      first_release_fds = service->open_file_count();
    if (release_calls == fail_release || fail_all_releases)
      return Status::io_error(release_calls == 1 ? "first release failure"
                                                 : "later release failure");
    return Status::success();
  }
  fs::Node *lookup(std::string_view name) override {
    return name == "file" ? child : nullptr;
  }
  Status create(std::string_view, fs::NodeType) override {
    return Status::unsupported();
  }
  Status chmod(u32) override { return Status::success(); }
  Status chown(u32, u32) override { return Status::success(); }
};

struct Fixture {
  CountedNode file, root;
  sched::Scheduler scheduler;
  fs::VirtualFileSystem vfs;
  driver::ConsoleDriver console;
  posix::PosixService service;
  Fixture() {
    root.type = fs::NodeType::directory;
    root.child = &file;
    CHECK(vfs.mount("/", root));
    CHECK(console.start());
    CHECK(service.initialize(vfs, console, scheduler));
    const auto process = scheduler.create_user_process(
        "path-reference", arch::configured_arch_operations().make_user_context(
                              {0x400000, 0x800000, 0}));
    CHECK(process && service.activate(process.value()));
    file.service = &service;
  }
  ~Fixture() {
    CHECK(file.pins == 0);
    CHECK(service.open_file_count() == 3 &&
          scheduler.open_description_count() == 3);
  }
  void empty() const {
    CHECK(file.pins == 0);
    CHECK(service.open_file_count() == 3 &&
          scheduler.open_description_count() == 3);
  }
  void reuse() {
    file.fail_retain = file.fail_release = file.fail_query = 0;
    file.fail_all_releases = file.fail_truncate = false;
    for (usize attempt = 0; attempt < 16; ++attempt) {
      const auto opened = service.open("/file", posix::o_RDONLY);
      CHECK(opened && opened.value() == 3 && file.pins == 1);
      CHECK(service.close(opened.value()));
      empty();
    }
  }
};

void move_and_release_once() {
  auto f = std::make_unique<Fixture>();
  {
    auto result = f->vfs.resolve({}, "/file");
    CHECK(result && f->file.pins == 1);
    fs::PathReference moved(std::move(result.value()));
    CHECK(result.value().node() == nullptr && moved.node() == &f->file);
    f->file.fail_release = 1;
    CHECK(!moved.release());
    CHECK(moved.node() == nullptr && f->file.pins == 0 &&
          f->file.release_calls == 1);
    CHECK(moved.release());
  }
  CHECK(f->file.release_calls == 1);
  f->reuse();
}

void retained_descriptor_and_close_failure() {
  auto f = std::make_unique<Fixture>();
  const auto first = f->service.open("/file", posix::o_RDWR);
  CHECK(first && first.value() == 3);
  CHECK(f->file.pins == 1 && f->file.retain_calls == 2 &&
        f->file.release_calls == 1);
  CHECK(f->file.first_release_fds == 3);
  const auto duplicate = f->service.duplicate(first.value());
  CHECK(duplicate && duplicate.value() == 4 && f->file.pins == 1);
  CHECK(f->service.close(first.value()) && f->file.pins == 1);
  f->file.fail_release = 2;
  CHECK(!f->service.close(duplicate.value()));
  f->empty();
  CHECK(f->file.release_calls == 2 && f->file.retain_calls == 2);
  f->reuse();
}

void release_failure_before_publication() {
  auto f = std::make_unique<Fixture>();
  f->file.fail_all_releases = true;
  const auto opened = f->service.open("/file", posix::o_RDWR);
  CHECK(!opened && opened.status().message() == "first release failure");
  CHECK(f->file.retain_calls == 2 && f->file.release_calls == 2);
  CHECK(f->file.first_release_fds == 3);
  f->empty();
  f->reuse();
}

void preserve_primary_errors() {
  for (usize retain_failure = 1; retain_failure <= 2; ++retain_failure) {
    auto f = std::make_unique<Fixture>();
    f->file.fail_retain = retain_failure;
    f->file.fail_all_releases = true;
    const auto opened = f->service.open("/file", posix::o_RDWR);
    CHECK(!opened && opened.status().message() == "primary retain failure");
    CHECK(f->file.release_calls == retain_failure - 1);
    f->empty();
    f->reuse();
  }
  {
    auto f = std::make_unique<Fixture>();
    f->file.fail_query = 2; // Resolver metadata succeeds; open metadata fails.
    f->file.fail_all_releases = true;
    const auto opened = f->service.open("/file", posix::o_RDWR);
    CHECK(!opened && opened.status().message() == "primary metadata failure");
    CHECK(f->file.retain_calls == 1 && f->file.release_calls == 1);
    f->empty();
    f->reuse();
  }
  {
    auto f = std::make_unique<Fixture>();
    f->file.fail_truncate = f->file.fail_all_releases = true;
    const auto opened =
        f->service.open("/file", posix::o_RDWR | posix::o_TRUNC);
    CHECK(!opened && opened.status().message() == "primary truncate failure");
    CHECK(f->file.retain_calls == 2 && f->file.release_calls == 2 &&
          f->file.truncate_calls == 1);
    f->empty();
    f->reuse();
  }
  {
    auto f = std::make_unique<Fixture>();
    f->file.fail_all_releases = true;
    const auto opened = f->service.open(
        "/file", posix::o_RDWR | posix::o_CREAT | posix::o_EXCL);
    CHECK(!opened && opened.status().code() == StatusCode::already_exists);
    CHECK(f->file.retain_calls == 1 && f->file.release_calls == 1);
    f->empty();
    f->reuse();
  }
}

int main() {
  move_and_release_once();
  retained_descriptor_and_close_failure();
  release_failure_before_publication();
  preserve_primary_errors();
  std::puts("PASS hosted path-reference cleanup: publish after release, pin "
            "accounting, first-error priority, fd reuse");
}
