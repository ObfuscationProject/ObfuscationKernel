#pragma once

// ext4 module facade: Recovery bindings and cold replay after interrupted mutation.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4_empty_orphan_recovery.hpp"
#include "ok/fs/ext4_namespace_recovery_binding.hpp"
#include "ok/fs/ext4_reclaim_recovery_binding.hpp"
#include "ok/fs/ext4_recovery.hpp"
#include "ok/fs/ext4_regular_move_recovery.hpp"
