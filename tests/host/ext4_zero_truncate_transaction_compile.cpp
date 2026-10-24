#include <ok/fs/ext4_zero_truncate_transaction.hpp>
using Session = ok::block::BlockSessions<2>::Session;
template class ok::fs::Ext4ZeroTruncateTransaction<Session>;
