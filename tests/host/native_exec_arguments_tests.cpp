#include "unavailable_realtime.hpp"
// Exec argument arena, #! interpreter resolution and ENOEXEC with the actual
// runtime loader units and a hosted execution backend; no guest execution.
#include "native_runtime_units.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace ok;
namespace native = ok::user::native;
namespace hw = ok::arch::execution;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::abort(); } } while (false)
namespace ok::arch {
Status initialize_clock() { return Status::success(); }
Result<u64> monotonic_nanoseconds() { return u64{3000000042}; }
u64 clock_resolution_ns() { return 1; }
}
namespace ok::arch::execution {
bool supported() noexcept { return false; }
OwnershipReport ownership_report() noexcept { return {}; }
Status initialize() { return Status::unsupported(); }
Result<AddressSpace> create_space(MappingView) { return Status::unsupported(); }
Status destroy_space(AddressSpace &space) { space = {}; return Status::success(); }
Status remap(const AddressSpace &, const PageMapping &) { return Status::success(); }
Result<Trap> enter(AddressSpace &, UserFrame &) { return Status::unsupported(); }
Result<Trap> enter_owned(AddressSpace &, UserFrame &, const KernelEntryStackView &) { return Status::unsupported(); }
UserFrame initial_frame(uptr pc, uptr sp) { UserFrame f{}; f.pc = pc; f.sp = sp; return f; }
Syscall syscall(const UserFrame &) { return {}; }
void set_return_value(UserFrame &frame, i64 value) { frame.registers[0] = static_cast<uptr>(value); }
bool user_range(const AddressSpace &, uptr, usize, bool) { return false; }
Status copy_from_user(const AddressSpace &, void *, uptr, usize) { return Status::fault(); }
Status copy_to_user(const AddressSpace &, uptr, const void *, usize) { return Status::fault(); }
}

namespace {
std::span<const std::byte> bytes(std::string_view text)
{ return {reinterpret_cast<const std::byte *>(text.data()), text.size()}; }

void arena()
{
    native::Arguments arguments;
    CHECK(arguments.argc() == 0 && arguments.envc() == 0 && arguments.string_bytes() == 0);
    // Far past the former 16 x 256-byte bound, growing the run as it fills.
    const std::string word(100, 'a');
    for (usize i = 0; i < 1000; ++i) CHECK(arguments.push_argv(word));
    for (usize i = 0; i < 100; ++i) CHECK(arguments.push_envp("NAME=value"));
    CHECK(arguments.argc() == 1000 && arguments.envc() == 100);
    CHECK(arguments.string_bytes() == 1000 * 101 + 100 * 11);
    CHECK(arguments.prepend_argv("first") && arguments.argv(0) == "first" && arguments.argv(1) == word);
    CHECK(arguments.replace_argv(1, "second") && arguments.argc() == 1001);
    CHECK(arguments.argv(0) == "first" && arguments.argv(1) == "second" && arguments.argv(2) == word);
    CHECK(arguments.envp(99) == "NAME=value");
    // Moving the arena keeps its strings; the source becomes empty.
    native::Arguments moved{std::move(arguments)};
    CHECK(moved.argc() == 1001 && moved.argv(1) == "second" && arguments.argc() == 0);

    native::Arguments limits;
    const std::string longest(native::Arguments::max_string_bytes - 1, 'x');
    CHECK(limits.push_argv(longest));
    const std::string too_long(native::Arguments::max_string_bytes, 'x');
    CHECK(limits.push_argv(too_long).code() == StatusCode::argument_list_too_long);
    CHECK(limits.push_envp(longest));
    CHECK(limits.push_envp(std::string(2, 'y')).code() == StatusCode::argument_list_too_long);
    CHECK(limits.argc() == 1 && limits.envc() == 1);
    native::Arguments many;
    for (usize i = 0; i < native::Arguments::max_strings / 2; ++i) CHECK(many.push_argv("z"));
    CHECK(many.push_argv("z").code() == StatusCode::argument_list_too_long);
    CHECK(syscall::ErrnoMapper::errno_for(Status::argument_list_too_long()) == 7);
    CHECK(syscall::ErrnoMapper::errno_for(Status::exec_format()) == 8);
}

void scripts(Kernel &kernel)
{
    CHECK(kernel.vfs().create_readonly_file("/interp", bytes("\x7f" "ELF"), 0755));
    CHECK(kernel.vfs().create_readonly_file("/one", bytes("#! /interp  -x -y \t\nexit\n"), 0755));
    CHECK(kernel.vfs().create_readonly_file("/two", bytes("#!/one\n"), 0755));
    CHECK(kernel.vfs().create_readonly_file("/bare", bytes("#!/interp\n"), 0755));
    CHECK(kernel.vfs().create_readonly_file("/empty", bytes("#!   \n"), 0755));
    CHECK(kernel.vfs().create_readonly_file("/text", bytes("echo plain\n"), 0755));
    CHECK(kernel.vfs().create_readonly_file("/hidden", bytes("#!/interp\n"), 0644));
    {
        FixedString<256> path("/one");
        native::Arguments arguments;
        CHECK(arguments.push_argv("one-name") && arguments.push_argv("a"));
        CHECK(native::resolve_scripts(kernel, path, arguments));
        // Linux passes the rest of the line, trimmed, as one argument.
        CHECK(path.view() == "/interp" && arguments.argc() == 4);
        CHECK(arguments.argv(0) == "/interp" && arguments.argv(1) == "-x -y");
        CHECK(arguments.argv(2) == "/one" && arguments.argv(3) == "a");
    }
    {
        FixedString<256> path("/two");
        native::Arguments arguments;
        CHECK(arguments.push_argv("two") && arguments.push_argv("b"));
        CHECK(native::resolve_scripts(kernel, path, arguments));
        CHECK(path.view() == "/interp" && arguments.argc() == 5);
        CHECK(arguments.argv(0) == "/interp" && arguments.argv(1) == "-x -y" && arguments.argv(2) == "/one");
        CHECK(arguments.argv(3) == "/two" && arguments.argv(4) == "b");
    }
    {
        FixedString<256> path("/bare");
        native::Arguments arguments;
        CHECK(native::resolve_scripts(kernel, path, arguments));
        CHECK(path.view() == "/interp" && arguments.argc() == 2 && arguments.argv(1) == "/bare");
    }
    {
        // Five nested interpreters exceed the Linux limit of four. Read-only
        // RAM files reference their bytes, so the bodies stay alive.
        static const std::array<std::string, 5> bodies{"#!/interp\n", "#!/nest0\n", "#!/nest1\n",
                                                        "#!/nest2\n", "#!/nest3\n"};
        for (int level = 0; level < 5; ++level)
            CHECK(kernel.vfs().create_readonly_file("/nest" + std::to_string(level), bytes(bodies[level]), 0755));
        FixedString<256> path("/nest3");
        native::Arguments arguments;
        CHECK(native::resolve_scripts(kernel, path, arguments) && path.view() == "/interp");
        FixedString<256> deep("/nest4");
        native::Arguments deeper;
        CHECK(native::resolve_scripts(kernel, deep, deeper).code() == StatusCode::too_many_symlinks);
    }
    {
        FixedString<256> path("/empty");
        native::Arguments arguments;
        CHECK(native::resolve_scripts(kernel, path, arguments).code() == StatusCode::exec_format);
        FixedString<256> hidden("/hidden");
        CHECK(native::resolve_scripts(kernel, hidden, arguments).code() == StatusCode::denied);
        FixedString<256> text("/text");
        CHECK(native::resolve_scripts(kernel, text, arguments) && text.view() == "/text");
        auto loaded = native::load(kernel, "/text", arguments);
        CHECK(!loaded && loaded.status().code() == StatusCode::exec_format);
    }
}
} // namespace

int main()
{
    static Kernel kernel;
    CHECK(kernel.boot({}));
    CHECK(kernel.posix().activate(0));
    arena();
    scripts(kernel);
    CHECK(native::page_pool.object_count() == 0);
    std::puts("PASS native exec arguments: arena, ARG_MAX, #! interpreters, ENOEXEC; hosted, no guest execution");
}
