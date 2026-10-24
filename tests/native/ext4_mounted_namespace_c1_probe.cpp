// Freestanding compile probe; execution requires a distinct native receipt.
#include <ok/fs/ext4_mounted_metadata.hpp>
using Mount=ok::fs::Ext4MountedMetadata<1>;
extern "C" ok::Status ext4_mounted_namespace_c1_probe(Mount &mount,Mount::Registry &registry,
    Mount::Registry::Registration registration,ok::block::SessionBounds bounds)
{
    const auto status=mount.mount(registry,registration,bounds);if(!status)return status;
    const auto report=mount.startup_report();
    if(report.initial_family!=ok::fs::Ext4StartupFamily::namespace_active || report.namespace_checkpoints!=1 ||
       report.reclaimed!=1 || report.initial_sequence!=20 || report.final_sequence!=25 ||
       !report.namespace_result_validated || !report.strict_ordinary_admission || mount.report().epoch!=1)
        return ok::Status::fault("mounted namespace C1 terminal state differs");
    return {};
}
