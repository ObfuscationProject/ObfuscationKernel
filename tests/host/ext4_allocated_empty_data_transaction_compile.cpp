#include <ok/fs/ext4_allocated_empty_data_transaction.hpp>
using Session = ok::block::BlockSessions<2>::Session;
template class ok::fs::Ext4AllocatedEmptyDataTransaction<Session>;
