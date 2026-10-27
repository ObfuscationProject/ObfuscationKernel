#include <ok/fs/ext4/mount.hpp>
using Validator = ok::fs::Ext4ZeroTruncateOwnershipValidator;
template <class T> concept PublicBirthRecord = requires { typename T::CompletedBirthSeal; };
template <class T> concept PublicCompletedVisitor = requires(T &owner) { owner.typed_completed_workspace_exact({}); };
template <class T> concept PublicBirthValidation = requires(T &child) { child.completed_ownership_birth_valid(); };
static_assert(!PublicBirthRecord<Validator>);
static_assert(!PublicCompletedVisitor<ok::fs::Ext4OwnedTransaction<2, 8>>);
static_assert(!PublicBirthValidation<ok::fs::Ext4ZeroTruncateTransaction<ok::block::BlockSessions<2>::Session>>);
static_assert(!PublicBirthValidation<ok::fs::Ext4AllocatedEmptyDataTransaction<ok::block::BlockSessions<2>::Session>>);
template class ok::fs::Ext4MountedMetadata<2>;
template class ok::fs::Ext4OwnedTransaction<2, 8>;
