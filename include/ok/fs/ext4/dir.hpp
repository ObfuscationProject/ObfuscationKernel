#pragma once

// ext4 module facade: Directory entries, namespace mutation, rename and regular-file moves.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4_directory_initialization_budget.hpp"
#include "ok/fs/ext4_namespace_completion.hpp"
#include "ok/fs/ext4_namespace_plan.hpp"
#include "ok/fs/ext4_namespace_result_history.hpp"
#include "ok/fs/ext4_namespace_transaction.hpp"
#include "ok/fs/ext4_namespace_witness.hpp"
#include "ok/fs/ext4_regular_move_completion.hpp"
#include "ok/fs/ext4_regular_move_plan.hpp"
#include "ok/fs/ext4_regular_move_transaction.hpp"
#include "ok/fs/ext4_regular_move_witness.hpp"
#include "ok/fs/ext4_rename_plan.hpp"
#include "ok/fs/ext4_rename_transaction.hpp"
