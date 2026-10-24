#pragma once

// ext4 module facade: Metadata transactions, plans, witnesses, completion, owned transactions, metadata buffers.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4_completion.hpp"
#include "ok/fs/ext4_metadata_transaction.hpp"
#include "ok/fs/ext4_owned_transaction.hpp"
#include "ok/fs/metadata_buffers.hpp"
