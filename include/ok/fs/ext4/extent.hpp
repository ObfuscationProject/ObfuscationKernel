#pragma once

// ext4 module facade: Extent-backed data blocks: first/next data, allocated-empty data, zero-length truncate.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4_allocated_empty_data_ownership.hpp"
#include "ok/fs/ext4_allocated_empty_data_plan.hpp"
#include "ok/fs/ext4_allocated_empty_data_preview.hpp"
#include "ok/fs/ext4_allocated_empty_data_session_preview.hpp"
#include "ok/fs/ext4_allocated_empty_data_transaction.hpp"
#include "ok/fs/ext4_allocated_empty_data_transaction_budget.hpp"
#include "ok/fs/ext4_allocated_empty_data_witness.hpp"
#include "ok/fs/ext4_blocks.hpp"
#include "ok/fs/ext4_direct_read_scope.hpp"
#include "ok/fs/ext4_first_data_plan.hpp"
#include "ok/fs/ext4_first_data_transaction.hpp"
#include "ok/fs/ext4_next_data_plan.hpp"
#include "ok/fs/ext4_next_data_transaction.hpp"
#include "ok/fs/ext4_zero_truncate_plan.hpp"
#include "ok/fs/ext4_zero_truncate_preview.hpp"
#include "ok/fs/ext4_zero_truncate_session_preview.hpp"
#include "ok/fs/ext4_zero_truncate_transaction.hpp"
#include "ok/fs/ext4_zero_truncate_transaction_budget.hpp"
#include "ok/fs/ext4_zero_truncate_witness.hpp"
