/*
 * Copyright 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#pragma once
#include "raft/raft.hh"
#include "db/commitlog/commitlog.hh"
#include <map>

namespace service::strong_consistency {
struct index_and_replay_position {
    raft::index_t index;
    db::rp_handle replay_position_handle;
};

// Using map to have the entries sorted by index, which allows efficient retrieval of all entries up to a given index.
using raft_index_to_replay_position_map = std::map<raft::index_t, db::rp_handle>;

struct replayed_data_per_group {
    raft_index_to_replay_position_map replay_positions;
    raft::log_entries entries;
};

// This class implements the persistence for raft log using database commit log.
// It is used by tablet raft groups to persist their log entries.
class commitlog_persistence {
private:
    const raft::group_id _group_id;
    const db::cf_id_type _table_id;
    // Common commit log.
    db::commitlog& _commit_log;
    // Replay positions in the commit log for each raft log entry.
    // Contains entries that have been added but not yet removed by either:
    //  - truncate_log() (leader change discarding uncommitted tail)
    //  - truncate_log_tail() (snapshot allowing old entries to be reclaimed)
    // After a snapshot, some entries with index below the snapshot index may
    // still be present, in accordance with raft trailing log settings.
    raft_index_to_replay_position_map _commit_log_replay_positions_map;
    // The log entries that were loaded from database commit log on startup.
    raft::log_entries _replayed_entries;

public:
    commitlog_persistence(raft::group_id group_id, db::commitlog& commit_log, table_id target_table_id, replayed_data_per_group replayed_data);

    ~commitlog_persistence();

    // Persist the given log entries in the commit log and get the replay position handles for them.
    future<> store_log_entries(const raft::log_entry_ptr_list& entries);

    // Get the log items that were loaded from database commit log on startup.
    raft::log_entries load_log();

    // Remove all the items with index >= idx, as they are considered truncated in Raft semantics.
    void truncate_log(raft::index_t idx);

    // Remove replay position handles for entries that have been snapshotted
    // and are no longer needed in the raft log. This allows the commitlog
    // segments holding those entries to be reclaimed.
    // Called from store_snapshot_descriptor after the snapshot is persisted.
    void truncate_log_tail(raft::index_t index);

    // Get cloned replay position handles for the specified indices.
    // The clones are handed to memtables in the raft state machine apply(),
    // while originals stay in the map to keep commitlog segments alive for raft log purposes.
    // Triggers on_internal_error if an entry is missing from the map.
    std::vector<index_and_replay_position> get_replay_position_handles_for(const raft::log_entry_ptr_list& entries);
};
} // namespace service::strong_consistency
