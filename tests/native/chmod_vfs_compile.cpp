// Compile the exported Node boundary without an incidental include-order dependency.
#include "ok/fs/vfs.hpp"

ok::fs::ModeChangeResult chmod_vfs_compile(ok::fs::Node &node, ok::u32 mode,
                                           ok::user::Credentials caller)
{
    return node.change_mode(mode, caller);
}
