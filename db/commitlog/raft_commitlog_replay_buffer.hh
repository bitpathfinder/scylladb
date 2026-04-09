/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#pragma once

#include <unordered_map>
#include <seastar/core/future.hh>
#include "utils/chunked_vector.hh"
#include "raft/raft.hh"
#include "db/commitlog/replay_position.hh"
#include "service/strong_consistency/commitlog_persistence.hh"

namespace db {
class system_keyspace;

// Result of checking entry ordering during commitlog replay.
// Used to detect leader changes, duplicates, and crash recovery scenarios.
enum class entry_ordering_check_result {
    // Entry is in order (idx > last_idx, or first entry)
    in_order,
    // Entry has smaller index but different term - indicates leader change,
    // previous entries with idx >= this entry's idx should be discarded
    leader_change,
    // Entry has same index and same term - this is a duplicate entry,
    // can occur after crash recovery when both old and new commitlog segments are replayed
    duplicate,
    // Entry has smaller index with same term - indicates we hit the start of
    // a duplicate range from an older commitlog segment (crash recovery scenario)
    out_of_order_same_term
};

// Check entry ordering relative to the last seen entry.
// Returns the appropriate action to take based on the entry's index and term.
//
// This function is used during commitlog replay to detect:
// - Normal sequential entries (in_order)
// - Leader changes where a new leader overwrites uncommitted entries (leader_change)
// - Duplicate entries from crash recovery (duplicate)
// - Duplicate tail from older segment during crash recovery (out_of_order_same_term)
//
// @param entry_idx Index of the current entry
// @param entry_term Term of the current entry
// @param last_idx Index of the previously processed entry (0 if this is the first)
// @param last_term Term of the previously processed entry
inline entry_ordering_check_result check_entry_ordering(raft::index_t entry_idx, raft::term_t entry_term, raft::index_t last_idx, raft::term_t last_term) {
    if (last_idx == raft::index_t{0}) {
        return entry_ordering_check_result::in_order;
    }
    if (entry_idx > last_idx) {
        return entry_ordering_check_result::in_order;
    }
    if (entry_idx == last_idx && entry_term == last_term) {
        return entry_ordering_check_result::duplicate;
    }
    if (entry_term != last_term) {
        return entry_ordering_check_result::leader_change;
    }
    return entry_ordering_check_result::out_of_order_same_term;
}

// Per-shard buffer that collects Raft log entries found in the commitlog
// during replay. Entries are grouped by group_id and later consumed by
// commitlog_persistence instances when tablet Raft groups are started.
//
// Lifecycle:
//   1. During commitlog replay, entries are added via add().
//   2. After replay completes, process_raft_replay_buffer() is called to:
//      - Apply committed mutations to memtables (so they get flushed to sstables)
//      - Rewrite uncommitted entries to the new commitlog (getting rp_handles)
//      - Discard entries that precede the snapshot index
//   3. When tablet raft groups start, each commitlog_persistence instance
//      consumes its group's entries via take_replayed_group_entries().
//
// Crash Recovery Handling:
//   After a crash, both old and new commitlog segments may be replayed, which
//   can result in seeing entries from the same Raft group multiple times or
//   in unexpected orders. The processing handles several scenarios:
//
//   1. Leader changes: When a new leader is elected, it may overwrite uncommitted
//      entries from the previous leader starting at some index. During replay,
//      this appears as entries with a higher term but lower/equal index. When
//      detected, all entries with idx >= the new entry's idx are discarded.
//
//   2. Superseded terms: After processing a leader change to term N, any entries
//      from terms < N that appear later in the replay (from old segments) are
//      skipped entirely, as they belong to terms that have been superseded.
//
//   3. Duplicate entries: Consecutive entries with the same (idx, term) pair
//      are skipped, as they represent the same logical entry written multiple
//      times (e.g., due to retry after timeout).
//
//   4. Snapshotted entries: Entries with idx <= snapshot.idx are NOT filtered out
//      during the first pass. They are applied to memtables if committed (because
//      after a crash, snapshotted data may not have been flushed to sstables), but
//      are excluded from the raft log (group_data.entries) since they are already
//      captured in the snapshot.
class raft_commitlog_replay_buffer {
    // The entries replayed from old commit log, grouped by raft group.
    // These are processed in-place by process_raft_replay_buffer() and then cleared.
    std::unordered_map<raft::group_id, utils::chunked_vector<raft::log_entry_ptr>> _replayed_commitlog_entries_by_group;

    // Resulting entries and rp_handles in the new commitlog that are handled to the appropriate
    // commitlog_persistence instances when tablet Raft groups start.
    std::unordered_map<raft::group_id, service::strong_consistency::replayed_data_per_group> _per_group_data;
    uint64_t _total_entries = 0;

public:
    // Add an entry during commitlog replay (before processing).
    void add(const raft::group_id group_id, raft::log_entry_ptr entry) {
        _replayed_commitlog_entries_by_group[group_id].push_back(std::move(entry));
        ++_total_entries;
    }

    // Get the replayed items for a group. These are removed from the buffer since they are now owned by the caller (commitlog_persistence).
    service::strong_consistency::replayed_data_per_group take_replayed_group_entries(const raft::group_id group_id) {
        auto it = _per_group_data.find(group_id);
        if (it == _per_group_data.end()) {
            return {};
        }
        service::strong_consistency::replayed_data_per_group result = std::move(it->second);
        _per_group_data.erase(it);
        return result;
    }

    uint64_t total_entries() const {
        return _total_entries;
    }

    // Number of groups that still have unconsumed entries.
    size_t remaining_groups() const {
        return _replayed_commitlog_entries_by_group.size();
    }

    future<> stop() {
        return make_ready_future<>();
    }

    // Process the raft replay items after commitlog replay completes but before
    // old commitlog segments are deleted and memtables are flushed.
    //
    // For each raft group in the buffer:
    //   1. Reads commit_idx and snapshot descriptor from the raft system tables.
    //   2. Filters entries by handling leader changes, duplicates, and superseded terms.
    //   3. For committed entries (idx <= commit_idx) that contain mutations:
    //      deserializes and applies them to memtables via apply_in_memory.
    //      This includes entries with idx <= snapshot.idx, because after a crash
    //      the snapshot data may not have been flushed to sstables.
    //   4. For uncommitted entries (idx > commit_idx): rewrites them to the new
    //      commitlog, obtaining real rp_handles that tie segment lifetime to the data.
    //   5. Only entries with idx > snapshot.idx are added to the raft log
    //      (group_data.entries) for later consumption by commitlog_persistence.
    //      Snapshotted entries don't need to be in the log for replication.
    //   6. Non-command entries (configuration, dummy) are kept in the raft log but
    //      don't need mutation application or commitlog rewrite.
    //
    // Must be called on each shard.
    future<> process_raft_replayed_items(replica::database& db, cql3::query_processor& qp, db::system_keyspace& sys_ks);
};
} // namespace db
