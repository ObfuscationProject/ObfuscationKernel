#include <ok/fs/ext4_mounted_metadata.hpp>
#include <type_traits>
using Mount = ok::fs::Ext4MountedMetadata<1>;
using Startup = ok::fs::Ext4MountedStartupWorkspace;
static_assert(!std::is_copy_constructible_v<Startup> && !std::is_move_constructible_v<Startup>);
extern "C" ok::Status ext4_mounted_startup_probe(Mount &mount, Mount::Registry &registry,
    Mount::Registry::Registration registration, ok::block::SessionBounds bounds)
{
    if (auto status = mount.mount(registry, registration, bounds); !status) return status;
    const auto report = mount.startup_report();
    if (!report.strict_ordinary_admission || report.phase != ok::fs::Ext4StartupPhase::complete ||
        report.observed.session_serial != report.original.session_serial)
        return ok::Status::fault("startup freestanding probe terminal admission differs");
    return mount.freeze_clean();
}
