// Normal-access freestanding instantiation covers the concrete suffix, its
// private Owner lineage helpers and both real typed children.
#include <ok/fs/ext4_mounted_metadata.hpp>
template class ok::fs::Ext4OwnedTransaction<2, 8>;
template class ok::fs::Ext4MountedMetadata<2>;
