#include "piix4_pm_poweroff.hpp"

#include <initializer_list>
#include <iostream>

namespace
{
using namespace ok;
using namespace ok::arch::detail;

struct Function final
{
    u32 identity = 0xffffffffu;
    u32 class_revision = 0xffffffffu;
    u32 header = 0xffffffffu;
    u32 base = 0xffffffffu;
    u32 decode = 0xffffffffu;
};

struct Read final
{
    u8 slot{};
    u8 function{};
    u8 offset{};
};

struct FakePorts final
{
    inline static Function functions[32][8]{};
    inline static Read reads[4096]{};
    inline static usize configuration_reads{};
    inline static usize control_reads{};
    inline static usize control_writes{};
    inline static u16 control = 1;
    inline static u16 read_ports[4]{};
    inline static u16 written_port{};
    inline static u16 written_value{};
    inline static bool invalid_access{};
    inline static bool mutation_done{};
    inline static void (*before_configuration_read)(u8, u8, u8) noexcept = nullptr;
    inline static void (*after_control_read)() noexcept = nullptr;

    static void reset() noexcept
    {
        for (auto &slot : functions)
            for (auto &function : slot) function = Function{};
        configuration_reads = control_reads = control_writes = 0;
        control = 1;
        written_port = written_value = 0;
        for (auto &port : read_ports) port = 0;
        invalid_access = mutation_done = false;
        before_configuration_read = nullptr;
        after_control_read = nullptr;
    }

    static void target(u8 slot = 1, u8 function = 3, u32 base = 0x601u) noexcept
    {
        if (function != 0)
            functions[slot][0] = Function{0x70008086u, 0x06010000u, 0x00800000u, 0, 0};
        functions[slot][function] = Function{0x71138086u, 0x06800003u, 0, base, 1};
    }

    static u32 read_configuration(u8 slot, u8 function, u8 offset) noexcept
    {
        if (configuration_reads < 4096)
            reads[configuration_reads] = Read{slot, function, offset};
        else
            invalid_access = true;
        ++configuration_reads;
        if (slot >= 32 || function >= 8 || (offset & 3u) != 0)
        {
            invalid_access = true;
            return 0xffffffffu;
        }
        if (before_configuration_read) before_configuration_read(slot, function, offset);
        const auto &entry = functions[slot][function];
        switch (offset)
        {
        case 0x00: return entry.identity;
        case 0x08: return entry.class_revision;
        case 0x0c: return entry.header;
        case 0x40: return entry.base;
        case 0x80: return entry.decode;
        default:
            invalid_access = true;
            return 0xffffffffu;
        }
    }

    static u16 read_control(u16 port) noexcept
    {
        if (control_reads < 4) read_ports[control_reads] = port;
        else invalid_access = true;
        ++control_reads;
        const auto value = control;
        if (after_control_read) after_control_read();
        return value;
    }

    static void write_control(u16 port, u16 value) noexcept
    {
        ++control_writes;
        written_port = port;
        written_value = value;
    }
};

using Policy = Piix4PmPoweroff<FakePorts, FakePorts>;

struct MissingConfiguration {};
struct WrongConfiguration final
{
    static u16 read_configuration(u8, u8, u8) noexcept;
};
struct ThrowingConfiguration final
{
    static u32 read_configuration(u8, u8, u8);
};
struct WrongControl final
{
    static u16 read_control(u16) noexcept;
    static bool write_control(u16, u16) noexcept;
};

static_assert(Piix4ConfigurationPorts<FakePorts>);
static_assert(Piix4PmControlPorts<FakePorts>);
static_assert(!Piix4ConfigurationPorts<MissingConfiguration>);
static_assert(!Piix4ConfigurationPorts<WrongConfiguration>);
static_assert(!Piix4ConfigurationPorts<ThrowingConfiguration>);
static_assert(!Piix4PmControlPorts<WrongControl>);

bool expect(bool condition, const char *message)
{
    if (!condition) std::cerr << "PIIX4 PM policy control failed: " << message << '\n';
    return condition;
}

bool no_write_failure(const char *message)
{
    const auto status = Policy::request();
    return expect(!status && FakePorts::control_writes == 0 && !FakePorts::invalid_access, message);
}

bool one_write(u16 port, u16 value, const char *message)
{
    const auto status = Policy::request();
    return expect(static_cast<bool>(status) && FakePorts::control_writes == 1 &&
                      FakePorts::control_reads == 2 && FakePorts::read_ports[0] == port &&
                      FakePorts::read_ports[1] == port && FakePorts::written_port == port &&
                      FakePorts::written_value == value && !FakePorts::invalid_access &&
                      FakePorts::configuration_reads <= 2 * Policy::maximum_probe_configuration_reads,
                  message);
}

bool no_followers_read(u8 slot)
{
    for (usize index = 0; index < FakePorts::configuration_reads; ++index)
        if (FakePorts::reads[index].slot == slot && FakePorts::reads[index].function != 0) return false;
    return true;
}

bool discovery_controls()
{
    bool passed = true;
    FakePorts::reset();
    FakePorts::target(0, 0);
    auto selected = Policy::probe();
    passed &= expect(selected && selected.value().slot == 0 && selected.value().function == 0 &&
                         selected.value().pm_base == 0x600 && selected.value().control_port == 0x604 &&
                         FakePorts::control_reads == 0 && FakePorts::control_writes == 0,
                     "normal function-zero discovery is read-only");

    FakePorts::reset();
    FakePorts::target(31, 7);
    selected = Policy::probe();
    passed &= expect(selected && selected.value().slot == 31 && selected.value().function == 7 &&
                         selected.value().header == 0 && !FakePorts::invalid_access,
                     "multifunction fn0 admits a normal nonzero function without its own multifunction bit");

    FakePorts::reset();
    FakePorts::target(1, 3);
    FakePorts::functions[1][0] = Function{};
    passed &= no_write_failure("orphan target with absent function zero is not enumerated");
    passed &= expect(no_followers_read(1) && FakePorts::configuration_reads == 32,
                     "absent function zero skips every following function");

    FakePorts::reset();
    FakePorts::target(1, 3);
    FakePorts::functions[1][0].header = 0;
    passed &= no_write_failure("single-function fn0 hides unsupported following functions");
    passed &= expect(no_followers_read(1), "single-function header bounds the actual scan");

    FakePorts::reset();
    FakePorts::target(1, 0);
    FakePorts::functions[1][0].header = 0x00800000u;
    FakePorts::functions[1][7] = Function{0x12348086u, 0, 0, 0, 0};
    passed &= one_write(0x604, 0x2001, "multifunction target at fn0 scans missing and present followers");

    for (const auto malformed : {0x12340000u, 0xffff8086u})
    {
        FakePorts::reset();
        FakePorts::target();
        FakePorts::functions[2][0] = Function{malformed, 0, 0, 0, 0};
        passed &= no_write_failure("malformed unrelated present identity fails closed");
    }
    FakePorts::reset();
    FakePorts::target();
    FakePorts::functions[2][0] = Function{0x12348086u, 0, 0x00030000u, 0, 0};
    passed &= no_write_failure("reserved unrelated header type fails closed");

    FakePorts::reset();
    FakePorts::target(1, 0);
    FakePorts::target(2, 0);
    passed &= no_write_failure("duplicate exact PM identities in different slots are ambiguous");
    FakePorts::reset();
    FakePorts::target(1, 3);
    FakePorts::functions[1][7] = FakePorts::functions[1][3];
    passed &= no_write_failure("duplicate exact PM identities in one multifunction slot are ambiguous");

    for (const auto identity : {0x71138087u, 0x71148086u})
    {
        FakePorts::reset();
        FakePorts::target();
        FakePorts::functions[1][3].identity = identity;
        passed &= no_write_failure("vendor and device must both match exactly");
    }
    for (const auto class_revision : {0x06800002u, 0x06800103u, 0x06810003u, 0x07800003u})
    {
        FakePorts::reset();
        FakePorts::target();
        FakePorts::functions[1][3].class_revision = class_revision;
        passed &= no_write_failure("class, subclass, programming interface and revision match exactly");
    }
    for (const auto type : {1u, 2u, 3u, 0x7fu})
    {
        FakePorts::reset();
        FakePorts::target();
        FakePorts::functions[1][3].header = type << 16;
        passed &= no_write_failure("PM candidate requires a normal type-zero header");
    }

    FakePorts::reset();
    for (u8 slot = 0; slot < 32; ++slot)
        for (u8 function = 0; function < 8; ++function)
            FakePorts::functions[slot][function] = Function{0x12348086u, 0, function == 0 ? 0x00800000u : 0, 0, 0};
    FakePorts::target(31, 7);
    selected = Policy::probe();
    passed &= expect(selected && FakePorts::configuration_reads == Policy::maximum_probe_configuration_reads &&
                         !FakePorts::invalid_access,
                     "fully populated bus uses the finite 522-read probe bound");
    return passed;
}

bool resource_controls()
{
    bool passed = true;
    for (const auto base : {0x00000001u, 0x00000041u, 0x000003c1u, 0x00000600u,
                            0x00000603u, 0x00000621u, 0x00000cc1u, 0x00010601u, 0xffffffc1u})
    {
        FakePorts::reset();
        FakePorts::target(1, 3, base);
        passed &= no_write_failure("zero, legacy, memory, unaligned, config-overlap or upper PMBASE bits refuse writes");
    }
    for (const auto base : {0x00000401u, 0x00000601u, 0x0000b001u, 0x0000ffc1u})
    {
        FakePorts::reset();
        FakePorts::target(1, 3, base);
        passed &= one_write(static_cast<u16>((base & 0xffc0u) + 4u), 0x2001,
                            "selected I/O window boundaries and discovered firmware bases are admitted");
    }
    for (const auto decode : {0u, 2u, 3u, 0xffu})
    {
        FakePorts::reset();
        FakePorts::target();
        FakePorts::functions[1][3].decode = decode;
        passed &= no_write_failure("decode low byte must be exactly one");
    }
    FakePorts::reset();
    FakePorts::target();
    FakePorts::functions[1][3].decode = 0xa55a0001u;
    passed &= one_write(0x604, 0x2001, "unrelated upper bytes of decode dword are not interpreted as the PM register");

    FakePorts::reset();
    FakePorts::target();
    const auto preflight = Policy::preflight();
    passed &= expect(static_cast<bool>(preflight) && FakePorts::configuration_reads > 0 &&
                         FakePorts::control_reads == 0 && FakePorts::control_writes == 0 &&
                         !FakePorts::invalid_access,
                     "preflight never reads or writes PM control and has no configuration-data write API");
    return passed;
}

bool fresh_request_controls()
{
    bool passed = true;
    FakePorts::reset();
    FakePorts::target();
    passed &= expect(static_cast<bool>(Policy::preflight()), "initial preflight succeeds");
    FakePorts::functions[1][3].base = 0xb001u;
    passed &= one_write(0xb004, 0x2001, "request discovers a valid moved base instead of using a preflight cache");

    for (unsigned mutation = 0; mutation < 5; ++mutation)
    {
        FakePorts::reset();
        FakePorts::target();
        passed &= expect(static_cast<bool>(Policy::preflight()), "pre-mutation preflight succeeds");
        switch (mutation)
        {
        case 0: FakePorts::functions[1][3] = Function{}; break;
        case 1: FakePorts::functions[1][3].decode = 0; break;
        case 2: FakePorts::functions[1][3].base = 0x603u; break;
        case 3: FakePorts::target(31, 0); break;
        case 4: FakePorts::functions[1][0].header = 0; break;
        }
        passed &= no_write_failure("disappearance, disable, malformed base, duplicate or lost multifunction after preflight refuses");
    }

    for (unsigned mutation = 0; mutation < 4; ++mutation)
    {
        FakePorts::reset();
        FakePorts::target();
        if (mutation == 0)
            FakePorts::after_control_read = []() noexcept {
                if (FakePorts::control_reads == 1) FakePorts::functions[1][3] = Function{};
            };
        if (mutation == 1)
            FakePorts::after_control_read = []() noexcept {
                if (FakePorts::control_reads == 1) FakePorts::functions[1][3].base = 0xb001u;
            };
        if (mutation == 2)
            FakePorts::after_control_read = []() noexcept {
                if (FakePorts::control_reads == 1) FakePorts::target(31, 0);
            };
        if (mutation == 3)
            FakePorts::after_control_read = []() noexcept {
                if (FakePorts::control_reads == 1) FakePorts::control = 3;
            };
        passed &= no_write_failure("full final reprobe and control reread refuse observed removal, movement, duplicate or control change");
    }

    FakePorts::reset();
    FakePorts::target();
    FakePorts::before_configuration_read = [](u8 slot, u8 function, u8 offset) noexcept {
        if (!FakePorts::mutation_done && FakePorts::control_reads == 1 && slot == 31 && function == 0 && offset == 0)
        {
            FakePorts::functions[1][3].base = 0xb001u;
            FakePorts::mutation_done = true;
        }
    };
    passed &= no_write_failure("selected-descriptor confirmation catches a base changed after its enumeration read");
    passed &= expect(FakePorts::mutation_done, "late final-scan mutation actually happened");

    FakePorts::reset();
    FakePorts::target();
    FakePorts::before_configuration_read = [](u8 slot, u8 function, u8 offset) noexcept {
        if (!FakePorts::mutation_done && FakePorts::control_reads == 1 && slot == 31 && function == 0 && offset == 0)
        {
            FakePorts::functions[1][0].header = 0;
            FakePorts::mutation_done = true;
        }
    };
    passed &= no_write_failure("selected-descriptor confirmation also includes the controlling function-zero header");

    FakePorts::reset();
    FakePorts::target();
    FakePorts::before_configuration_read = [](u8 slot, u8 function, u8 offset) noexcept {
        if (!FakePorts::mutation_done && FakePorts::control_reads == 1 && slot == 31 && function == 0 && offset == 0)
        {
            FakePorts::functions[1][3].decode = 0;
            FakePorts::mutation_done = true;
        }
    };
    passed &= no_write_failure("last selected configuration read observes decode disabled during final scan");
    return passed;
}

bool control_word_controls()
{
    bool passed = true;
    struct ControlWord final
    {
        u16 before;
        u16 after;
    };
    constexpr ControlWord words[] = {{0x0000, 0x2000}, {0x0001, 0x2001}, {0x03ff, 0x23ff},
                                     {0xc001, 0xe001}, {0xdcff, 0xe0ff}};
    for (const auto word : words)
    {
        FakePorts::reset();
        FakePorts::target();
        FakePorts::control = word.before;
        passed &= one_write(0x604, word.after,
                            "one 16-bit S5 type-zero write preserves SCI and all unrelated control bits");
    }
    for (const auto control : {u16{0xffff}, u16{0x2000}, u16{0x3c01}})
    {
        FakePorts::reset();
        FakePorts::target();
        FakePorts::control = control;
        passed &= no_write_failure("unmapped control or an already pending sleep request refuses any write");
    }
    FakePorts::reset();
    FakePorts::target();
    FakePorts::after_control_read = []() noexcept {
        if (FakePorts::control_reads == 1) FakePorts::control = 0xffff;
    };
    passed &= no_write_failure("unavailable final control read refuses any write");
    return passed;
}
} // namespace

int main()
{
    bool passed = discovery_controls();
    passed &= resource_controls();
    passed &= fresh_request_controls();
    passed &= control_word_controls();
    if (!passed) return 1;
    std::cout << "PIIX4 PM hosted policy controls passed; no privileged I/O, guest execution, or hardware qualification\n";
    return 0;
}
