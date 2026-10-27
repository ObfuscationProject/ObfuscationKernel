#include <ok/fs/ext4/txn.hpp>

using namespace ok;
using namespace ok::fs;

Status compile_allocated_empty_session_preview(block::BlockSessions<4>::Session &session,
    const Ext4RecoveryInfo &binding,
    Ext4AllocatedEmptyDataSessionPreview<block::BlockSessions<4>::Session> &preview)
{ return preview.prepare(session, binding); }
