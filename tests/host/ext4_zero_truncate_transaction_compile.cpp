#include <ok/fs/ext4/txn.hpp>
using Session = ok::block::BlockSessions<2>::Session;
template class ok::fs::Ext4ZeroTruncateTransaction<Session>;
