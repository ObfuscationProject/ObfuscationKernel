#pragma once

// ext4 module facade: Inode/block allocation, creation plans, orphan chains and reclaim.
//
// Step 1 of refactor/ext4-layout: callers include this facade instead of the
// individual slice headers below. Step 2 folds the slices into this header and
// deletes them; scripts/check_ext4_layout.py enforces that every slice header
// belongs to exactly one facade until then.

#include "ok/fs/ext4_create_plan.hpp"
#include "ok/fs/ext4_create_transaction.hpp"
#include "ok/fs/ext4_create_witness.hpp"
#include "ok/fs/ext4_directory_create_plan.hpp"
#include "ok/fs/ext4_directory_create_sdk_profile.hpp"
#include "ok/fs/ext4_directory_create_witness.hpp"
#include "ok/fs/ext4_free_inode.hpp"
#include "ok/fs/ext4_inode_lifetime.hpp"
#include "ok/fs/ext4_orphan_access.hpp"
#include "ok/fs/ext4_orphan_chain.hpp"
#include "ok/fs/ext4_orphan_chain_capture.hpp"
#include "ok/fs/ext4_orphan_directory_qualified_reclaim.hpp"
#include "ok/fs/ext4_orphan_directory_refs.hpp"
#include "ok/fs/ext4_orphan_ownership.hpp"
#include "ok/fs/ext4_orphan_reclaim_capture.hpp"
#include "ok/fs/ext4_orphan_reclaim_cold.hpp"
#include "ok/fs/ext4_orphan_reclaim_plan.hpp"
#include "ok/fs/ext4_orphan_reclaim_witness.hpp"
#include "ok/fs/ext4_orphan_snapshot.hpp"
#include "ok/fs/ext4_reclaim_composed_budget.hpp"
#include "ok/fs/ext4_reclaim_composed_history.hpp"
#include "ok/fs/ext4_reclaim_history.hpp"
#include "ok/fs/ext4_reclaim_result_history.hpp"
#include "ok/fs/ext4_reclaim_route_budget.hpp"
