#include <ok/fs/ext4_metadata_transaction.hpp>
static_assert(!std::is_copy_constructible_v<ok::fs::Ext4MetadataTransaction>);
static_assert(!std::is_move_constructible_v<ok::fs::Ext4MetadataTransaction>);
ok::Status compile_transaction(ok::driver::BlockDevice &device, ok::fs::Ext4MetadataTransaction &writer,
                               ok::fs::Ext4RecoveryBounds bounds, ok::fs::Ext4InodeMetadataUpdate update)
{ return writer.commit(device, bounds, update); }
