#include "kernel_roadmap_tests.hpp"

#include "ok/memory/memory.hpp"
#include "ok/syscall/syscall.hpp"

#include <array>
#include <span>

namespace ok
{
namespace
{

uptr vm_test_mapping_address(arch::Architecture architecture)
{
    switch (architecture)
    {
    case arch::Architecture::i386:
    case arch::Architecture::arm32:
    case arch::Architecture::rv32:
    case arch::Architecture::mips:
    case arch::Architecture::ppc:
        return static_cast<uptr>(0xc000'0000u);
    case arch::Architecture::x86_64:
    case arch::Architecture::aarch64:
    case arch::Architecture::rv64:
    case arch::Architecture::loongarch64:
    case arch::Architecture::mips64:
        return static_cast<uptr>(0xffff'8000'0000'0000ull);
    }
    return static_cast<uptr>(0xc000'0000u);
}

class FrameReleaser final
{
  public:
    explicit FrameReleaser(memory::FrameAllocator &frames) : frames_(&frames)
    {
    }

    ~FrameReleaser()
    {
        if (frames_ == nullptr)
        {
            return;
        }
        for (usize i = 0; i < count_; ++i)
        {
            static_cast<void>(frames_->release(owned_[i]));
        }
    }

    Status keep(memory::PhysicalFrame frame)
    {
        if (count_ >= owned_.size())
        {
            return Status::overflow("VM test frame tracker capacity exceeded");
        }
        owned_[count_++] = frame;
        return Status::success();
    }

  private:
    memory::FrameAllocator *frames_{nullptr};
    std::array<memory::PhysicalFrame, 4> owned_{};
    usize count_{0};
};

Status verify_kernel_mapping(Kernel &kernel, memory::PhysicalFrame frame)
{
    memory::KernelAddressSpace kernel_space;
    const auto kernel_test_address = vm_test_mapping_address(kernel.arch().architecture());
    if (auto status = kernel_space.map_page(kernel_test_address, frame, memory::page_read | memory::page_write);
        !status.ok())
    {
        return status;
    }
    if (kernel_space.lookup(kernel_test_address) == nullptr || kernel_space.mapping_count() != 1)
    {
        return Status::fault("kernel page mapping was not visible");
    }
    if (auto status = kernel_space.unmap_page(kernel_test_address); !status.ok())
    {
        return status;
    }
    if (kernel_space.lookup(kernel_test_address) != nullptr || kernel_space.mapping_count() != 0)
    {
        return Status::fault("kernel page unmap did not remove mapping");
    }
    return Status::success();
}

Status verify_user_mapping_edges(memory::UserAddressSpace &user_space)
{
    if (!user_space.valid(memory::UserSlice<const std::byte>{.address = 0x400000, .count = 1}, memory::page_read))
    {
        return Status::fault("valid mapped user pointer was rejected");
    }
    if (user_space.valid(memory::UserSlice<const std::byte>{.address = 0, .count = 1}, memory::page_read))
    {
        return Status::fault("null user pointer was accepted");
    }
    if (user_space.valid(memory::UserSlice<const std::byte>{.address = 0x500000, .count = 1}, memory::page_read))
    {
        return Status::fault("unmapped user pointer was accepted");
    }

    std::array<std::byte, 1> byte{};
    if (user_space.copy_from_user(memory::UserSlice<const std::byte>{.address = 0, .count = 1}, byte)
            .status.code() != StatusCode::invalid_argument)
    {
        return Status::fault("null user pointer did not return a stable invalid-argument result");
    }
    if (user_space.copy_from_user(memory::UserSlice<const std::byte>{.address = 0x500000, .count = 1}, byte)
            .status.code() != StatusCode::fault)
    {
        return Status::fault("unmapped user pointer did not return a stable fault result");
    }
    return Status::success();
}

Status verify_user_copy_helpers(memory::UserAddressSpace &user_space)
{
    constexpr std::string_view text{"hello"};
    std::array<std::byte, 6> source{};
    for (usize i = 0; i < text.size(); ++i)
    {
        source[i] = static_cast<std::byte>(text[i]);
    }
    source[text.size()] = std::byte{0};

    auto written = user_space.copy_to_user(memory::UserSlice<std::byte>{.address = 0x400010, .count = source.size()},
                                           source);
    if (!written.ok() || written.bytes != source.size())
    {
        return Status::fault("copy_to_user failed");
    }

    std::array<std::byte, 6> out{};
    auto read = user_space.copy_from_user(memory::UserSlice<const std::byte>{.address = 0x400010, .count = out.size()},
                                          out);
    if (!read.ok() || read.bytes != out.size() || out != source)
    {
        return Status::fault("copy_from_user failed");
    }

    auto copied_string = user_space.copy_c_string_from_user(memory::UserPtr<const char>{.address = 0x400010}, 16);
    if (!copied_string || copied_string.value().view() != text)
    {
        return Status::fault("bounded user string copy failed");
    }

    constexpr std::array<u32, 3> words{0x11, 0x22, 0x33};
    auto word_bytes = std::span<const std::byte>{reinterpret_cast<const std::byte *>(words.data()),
                                                 words.size() * sizeof(u32)};
    if (!user_space.copy_to_user(memory::UserSlice<std::byte>{.address = 0x400100, .count = word_bytes.size()},
                                 word_bytes)
             .ok())
    {
        return Status::fault("copy_to_user for vector source failed");
    }
    std::array<u32, 3> copied_words{};
    auto vector_copy = user_space.copy_vector_from_user(memory::UserSlice<const u32>{.address = 0x400100,
                                                                                    .count = copied_words.size()},
                                                        std::span<u32>{copied_words});
    if (!vector_copy.ok() || copied_words != words)
    {
        return Status::fault("copy_vector_from_user failed");
    }
    return Status::success();
}

Status verify_safe_syscall_user_copy(memory::UserAddressSpace &user_space)
{
    syscall::Table table;
    struct CopyContext
    {
        const memory::UserAddressSpace *space;
    } context{.space = &user_space};
    if (auto status = table.register_callback(syscall::Number::ok_debug, "copy-cstr", &context,
                                              [](void *raw, const syscall::Request &request) {
                                                  auto *ctx = static_cast<CopyContext *>(raw);
                                                  auto copied = ctx->space->copy_c_string_from_user(
                                                      memory::UserPtr<const char>{
                                                          .address = static_cast<uptr>(request.args[0]),
                                                      },
                                                      static_cast<usize>(request.args[1]));
                                                  return syscall::Response{
                                                      .value = copied ? static_cast<i64>(copied.value().view().size())
                                                                      : -1,
                                                      .status = copied ? Status::success() : copied.status(),
                                                  };
                                              });
        !status.ok())
    {
        return status;
    }
    auto response = table.dispatch(syscall::Request{
        .number = syscall::Number::ok_debug,
        .args = {0x400010, 16, 0, 0, 0, 0},
    });
    if (!response.status.ok() || response.value != 5)
    {
        return Status::fault("safe UserPtr syscall dispatch failed");
    }
    return Status::success();
}

} // namespace

Status run_vm_roadmap_tests(Kernel &kernel, KernelTestReport &report)
{
    FrameReleaser release_frames{kernel.memory().frames()};
    auto kernel_frame = kernel.memory().frames().allocate();
    auto user_frame = kernel.memory().frames().allocate();
    auto readonly_frame = kernel.memory().frames().allocate();
    if (!kernel_frame || !user_frame || !readonly_frame)
    {
        return !kernel_frame   ? kernel_frame.status()
               : !user_frame   ? user_frame.status()
                                : readonly_frame.status();
    }
    if (auto status = release_frames.keep(kernel_frame.value()); !status.ok())
    {
        return status;
    }
    if (auto status = release_frames.keep(user_frame.value()); !status.ok())
    {
        return status;
    }
    if (auto status = release_frames.keep(readonly_frame.value()); !status.ok())
    {
        return status;
    }
    if (auto status = verify_kernel_mapping(kernel, kernel_frame.value()); !status.ok())
    {
        return status;
    }

    memory::UserAddressSpace user_space;
    if (auto status = user_space.map_page(0x400000, user_frame.value(),
                                          memory::page_read | memory::page_write | memory::page_user);
        !status.ok())
    {
        return status;
    }
    if (auto status = user_space.map_page(0x401000, readonly_frame.value(), memory::page_read | memory::page_user);
        !status.ok())
    {
        return status;
    }
    if (auto status = verify_user_mapping_edges(user_space); !status.ok())
    {
        return status;
    }
    if (auto status = verify_user_copy_helpers(user_space); !status.ok())
    {
        return status;
    }
    if (user_space.copy_to_user(memory::UserSlice<std::byte>{.address = 0x401000, .count = 1},
                                std::span<const std::byte>{reinterpret_cast<const std::byte *>("x"), 1})
            .status.code() != StatusCode::denied)
    {
        return Status::fault("write into read-only user mapping was not rejected");
    }

    memory::UserAddressSpace cloned_space;
    if (auto status = cloned_space.clone_metadata_from(user_space); !status.ok())
    {
        return status;
    }
    if (cloned_space.lookup(0x400000) == nullptr || cloned_space.lookup(0x401000) == nullptr)
    {
        return Status::fault("user address-space clone did not preserve mapping metadata");
    }

    if (auto status = user_space.mark_copy_on_write(0x400000); !status.ok())
    {
        return status;
    }
    if (user_space.copy_to_user(memory::UserSlice<std::byte>{.address = 0x400010, .count = 1},
                                std::span<const std::byte>{reinterpret_cast<const std::byte *>("!"), 1})
            .status.code() != StatusCode::denied)
    {
        return Status::fault("copy-on-write placeholder did not protect writes");
    }
    if (memory::classify_page_fault(true, true, true, false, memory::page_read | memory::page_user) !=
        memory::PageFaultKind::write_to_read_only)
    {
        return Status::fault("page fault write classification failed");
    }
    if (memory::classify_page_fault(false, false, true, false, 0) != memory::PageFaultKind::not_present)
    {
        return Status::fault("page fault not-present classification failed");
    }

    memory::VirtualMemoryManager vmm;
    if (auto status = vmm.initialize(kernel.memory().frames()); !status.ok())
    {
        return status;
    }
    auto allocated_user_frame = vmm.allocate_user_frame();
    if (!allocated_user_frame)
    {
        return allocated_user_frame.status();
    }
    if (auto status = kernel.memory().frames().release(allocated_user_frame.value()); !status.ok())
    {
        return status;
    }

    if (auto status = verify_safe_syscall_user_copy(user_space); !status.ok())
    {
        return status;
    }
    if (auto status = user_space.unmap_page(0x401000); !status.ok())
    {
        return status;
    }
    if (user_space.valid(memory::UserSlice<const std::byte>{.address = 0x401000, .count = 1}, memory::page_read))
    {
        return Status::fault("user page unmap did not remove mapping");
    }
    if (auto status = user_space.unmap_page(0x400000); !status.ok())
    {
        return status;
    }

    report.vm = true;
    return Status::success();
}

} // namespace ok
