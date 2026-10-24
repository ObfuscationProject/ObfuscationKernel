#pragma once

// ext4 module facade: JBD2 descriptor/replay model and journalled variants of the mutation slices.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4_allocated_empty_data_journal.hpp"
#include "ok/fs/ext4_allocated_empty_data_journal_verify.hpp"
#include "ok/fs/ext4_namespace_cold_replay.hpp"
#include "ok/fs/ext4_namespace_journal.hpp"
#include "ok/fs/ext4_namespace_replay.hpp"
#include "ok/fs/ext4_orphan_reclaim_journal.hpp"
#include "ok/fs/ext4_regular_move_journal.hpp"
#include "ok/fs/ext4_regular_move_replay.hpp"
#include "ok/fs/ext4_replay_shape.hpp"
#include "ok/fs/ext4_replay_view.hpp"
#include "ok/fs/ext4_zero_truncate_journal.hpp"
#include "ok/fs/jbd2.hpp"
#include "ok/fs/jbd2_replay.hpp"
