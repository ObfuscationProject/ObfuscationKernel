#pragma once

// ext4 module facade: On-disk layout, superblock/group descriptors, read-only access, timestamps.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4.hpp"
#include "ok/fs/ext4_fs.hpp"
#include "ok/fs/ext4_readonly.hpp"
#include "ok/fs/ext4_times.hpp"
