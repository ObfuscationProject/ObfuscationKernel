#include <ok/fs/ext4_mounted_metadata.hpp>

template class ok::fs::Ext4MountedMetadata<4>;
using Owner = ok::fs::Ext4OwnedTransaction<1, 8>;

ok::Status next_data_compile_probe(Owner &owner, const std::array<ok::u8, 1024> &image)
{
    if (auto status = owner.prepare_next_data({14, 1024, 1047, 1700000300, 234567891},
                                              1031, image); !status) return status;
    if (auto status = owner.commit(); !status) return status;
    return owner.checkpoint();
}
