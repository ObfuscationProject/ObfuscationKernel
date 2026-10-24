#pragma once

// ext4 module facade: Mounted-instance startup, common opening, typed cursors and retention.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4_mounted_common_opening_budget.hpp"
#include "ok/fs/ext4_mounted_common_profile.hpp"
#include "ok/fs/ext4_mounted_metadata.hpp"
#include "ok/fs/ext4_mounted_startup_workspace.hpp"
#include "ok/fs/ext4_mounted_typed_budget.hpp"
#include "ok/fs/ext4_mounted_typed_cursor.hpp"
