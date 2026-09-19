#include "ok/core/kernel.hpp"
#include "ok/user/runtime.hpp"
#include <cstdio>
#include <cstdlib>

using namespace ok;
#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!static_cast<bool>(condition))                                                                             \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                       \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

class ServiceExtension final : public KernelExtension
{
  public:
    unsigned calls{};
    Status observe(Kernel &kernel)
    {
        CHECK(kernel.posix().getpid() == 0);
        ++calls;
        return kernel.posix().set_identity(1000, 1000);
    }
    Status start(Kernel &kernel) override
    {
        return observe(kernel);
    }
    Status refresh(Kernel &kernel, bool) override
    {
        return observe(kernel);
    }
    Status surface_action(Kernel &kernel, gui::SurfaceId, ExtensionSurfaceAction) override
    {
        return observe(kernel);
    }
    Result<bool> key(Kernel &kernel, int) override
    {
        CHECK(observe(kernel));
        return true;
    }
    Result<bool> pointer_click(Kernel &kernel, i32, i32) override
    {
        CHECK(observe(kernel));
        return true;
    }
};

int main()
{
    static Kernel kernel;
    static ServiceExtension extension;
    CHECK(kernel.boot({.extension = &extension}));
    auto result = kernel.scheduler().create_background_process("identity-sentinel",
                                                               kernel.arch().make_kernel_context(0x1000, 0x8000));
    CHECK(result);
    const auto pid = result.value();
    const auto identity = kernel.scheduler().find(pid)->credentials();
    auto select = [&] { CHECK(kernel.posix().activate(pid)); };
    auto preserved = [&] {
        CHECK(kernel.posix().getpid() == 0);
        const auto *process = kernel.scheduler().find(pid);
        CHECK(process && process->credentials().kernel_space == identity.kernel_space &&
              process->credentials().euid == identity.euid && process->credentials().egid == identity.egid);
    };

    select();
    CHECK(kernel.start_extension());
    preserved();
    select();
    CHECK(kernel.handle_gui_key('x'));
    preserved();
    select();
    CHECK(kernel.handle_gui_mouse_position(2, 2, true));
    preserved();
    CHECK(extension.calls == 3);

    auto &compositor = kernel.gui().compositor();
    auto surface = compositor.create_surface({100, 100, 240, 160}, "extension-window");
    CHECK(surface);
    CHECK(compositor.maximize_surface(surface.value()));
    select();
    CHECK(kernel.handle_gui_mouse(0, 0, false));
    preserved();
    CHECK(extension.calls == 4);

    select();
    CHECK(kernel.handle_gui_scroll(1));
    preserved();
    select();
    CHECK(kernel.tick());
    preserved();
    select();
    CHECK(!user::native::step(kernel));
    preserved();
    select();
    CHECK(kernel.scheduler().block_current(sched::ThreadWaitReason::child, 77));
    CHECK(!user::native::step(kernel));
    preserved();
    const auto *blocked = kernel.scheduler().find(pid);
    CHECK(blocked->state() == sched::ProcessState::blocked &&
          blocked->threads()[0].wait_reason == sched::ThreadWaitReason::child && blocked->threads()[0].wait_key == 77);
    static fs::VirtualFileSystem selected_root;
    CHECK(selected_root.create("/selected", fs::NodeType::regular));
    CHECK(kernel.posix().set_identity(0, 0) && kernel.posix().chdir("/"));
    auto covered = kernel.posix().open("/", posix::o_RDONLY | posix::o_DIRECTORY);
    CHECK(covered && kernel.vfs().mount("/", *selected_root.lookup("/")));
    CHECK(kernel.posix().current_directory().status().code() == StatusCode::not_found);
    CHECK(kernel.posix().chdir("/"));
    auto pwd = kernel.debug_shell().execute("pwd");
    CHECK(pwd && pwd.value() == "/\n");
    auto selected = kernel.posix().open("selected", posix::o_RDONLY);
    CHECK(selected && kernel.posix().close(selected.value()));
    CHECK(kernel.posix().openat(covered.value(), "selected", posix::o_RDONLY).status().code() == StatusCode::not_found);
    CHECK(kernel.posix().close(covered.value()));
    std::puts(
        "PASS kernel services: GUI callbacks, window events, tick, native step identity and selected-root cwd");
}
