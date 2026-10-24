#include <ok/fs/ext4_owned_transaction.hpp>
using Owner = ok::fs::Ext4OwnedTransaction<>;
ok::Status qualify_ext4_owned(Owner &owner, Owner::Registry &registry, Owner::Registry::Registration key,
                              ok::block::SessionBounds bounds, ok::fs::Ext4InodeMetadataUpdate update,
                              Owner::Pin &pin, std::span<std::byte> bytes)
{
    if (auto status = owner.open(registry, key, bounds); !status) return status;
    if (auto status = owner.prepare(update); !status) return status;
    if (auto status = owner.pin(1, pin); !status) return status;
    if (auto status = pin.read(0, bytes); !status) return status;
    if (auto status = pin.release(); !status) return status;
    if (auto status = owner.commit(); !status) return status;
    if (auto status = owner.checkpoint(); !status) return status;
    return owner.close();
}
