#include <ok/fs/ext4/mount.hpp>
#include <type_traits>
using Mount = ok::fs::Ext4MountedMetadata<1>;
static_assert(!std::is_copy_constructible_v<ok::fs::Ext4NamespaceResultHistory> &&
    !std::is_move_constructible_v<ok::fs::Ext4NamespaceResultHistory>);
extern "C" ok::Status ext4_namespace_result_history_probe(Mount &mount, Mount::Registry &registry,
    Mount::Registry::Registration registration, ok::block::SessionBounds bounds)
{
    const auto status = mount.mount(registry, registration, bounds);
    const auto report = mount.startup_report();
    if (status.code() != ok::StatusCode::unsupported || !report.namespace_result_validated ||
        report.initial_family != ok::fs::Ext4StartupFamily::namespace_active || report.prospective_members != 1 ||
        report.namespace_checkpoints || report.strict_ordinary_admission || mount.root() || mount.report().epoch)
        return ok::Status::fault("prospective namespace probe distinction differs");
    return {};
}
