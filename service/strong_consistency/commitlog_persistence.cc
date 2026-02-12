/*
 * Copyright 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#include <seastar/core/when_all.hh>
#include <seastar/core/on_internal_error.hh>
#include "db/commitlog/commitlog.hh"
#include "raft/raft.hh"

#include "commitlog_persistence.hh"

#include "idl/commitlog.dist.hh"
#include "idl/commitlog.dist.impl.hh"
#include "idl/raft_storage.dist.hh"
#include "idl/raft_storage.dist.impl.hh"

namespace service::strong_consistency {
namespace {
seastar::logger logger("commitlog_persistence");
}

commitlog_persistence::commitlog_persistence(
        raft::group_id group_id, db::commitlog& commit_log, table_id target_table_id, replayed_data_per_group replayed_data)
    : _group_id(group_id)
    , _table_id(target_table_id)
    , _commit_log(commit_log)
    , _commit_log_replay_positions_map(std::move(replayed_data.replay_positions))
    , _replayed_entries(std::move(replayed_data.entries)) {
    logger.debug("starting commitlog_persistence group_id={}, table id {}, replayed entries={}", _group_id, _table_id, _replayed_entries.size());
}

commitlog_persistence::~commitlog_persistence() {
    // Release all remaining replay position handles without decrementing
    // segment dirty counts. This keeps commitlog segments alive after this
    // object is destroyed, ensuring uncommitted raft entries remain
    // available for replay after restart.
    // For committed-but-not-yet-snapshotted entries, the cloned handle in
    // the memtable still keeps the segment alive independently.
    for (auto& [idx, handle] : _commit_log_replay_positions_map) {
        handle.release();
    }
    logger.debug("released {} replay position handles for group_id={}", _commit_log_replay_positions_map.size(), _group_id);
}

seastar::future<> commitlog_persistence::store_log_entries(const raft::log_entry_ptr_list& entries) {
    logger.debug("store_log_entries: group_id={}, num_entries={}", _group_id, entries.size());

    std::vector<future<db::rp_handle>> pending_futures;
    pending_futures.reserve(entries.size());
    std::vector<commitlog_raft_log_entry_writer> writers;
    writers.reserve(entries.size());
    for (const auto& log_entry_ptr : entries) {
        logger.debug("  storing log entry: idx={}, term={}", log_entry_ptr->idx, log_entry_ptr->term);

        writers.emplace_back(raft_commit_log_entry{.group_id = _group_id, .entry = log_entry_ptr});
        auto write_entry_function = [&writer = writers.back()](auto& out) {
            return writer.write(out);
        };
        const auto target_size = writers.back().size();
        pending_futures.emplace_back(_commit_log.add(_table_id, target_size, db::no_timeout, db::commitlog_force_sync::yes, std::move(write_entry_function)));
    }
    auto replay_handles = co_await when_all_succeed(pending_futures.begin(), pending_futures.end());

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& log_entry_ptr = entries[i];
        _commit_log_replay_positions_map.emplace(log_entry_ptr->idx, std::move(replay_handles[i]));
    }
    logger.debug("store_log_entries completed: total_entries_in_map={}", _commit_log_replay_positions_map.size());
}

raft::log_entries commitlog_persistence::load_log() {
    logger.debug("load_log: group_id={}", _group_id);
    return std::move(_replayed_entries);
}

void commitlog_persistence::truncate_log(const raft::index_t idx) {
    logger.debug("truncate_log: group_id={}, idx={}", _group_id, idx);
    // Remove entries with index >= idx (Raft semantics: truncate from idx onward).
    _commit_log_replay_positions_map.erase(_commit_log_replay_positions_map.lower_bound(idx), _commit_log_replay_positions_map.end());
}

void commitlog_persistence::truncate_log_tail(const raft::index_t index) {
    logger.debug("truncate_log_tail: group_id={}, index={}", _group_id, index);
    // Remove entries with index <= the given index. The handles are destructed
    // normally, decrementing segment dirty counts and allowing commitlog
    // segments to be reclaimed once no other references hold them.
    auto end = _commit_log_replay_positions_map.upper_bound(index);
    _commit_log_replay_positions_map.erase(_commit_log_replay_positions_map.begin(), end);
    logger.debug("truncate_log_tail completed: remaining_map_size={}", _commit_log_replay_positions_map.size());
}

std::vector<index_and_replay_position> commitlog_persistence::get_replay_position_handles_for(const raft::log_entry_ptr_list& entries) {
    logger.debug("get_replay_position_handles_for: group_id={}, entries_count={}, current_map_size={}", _group_id, entries.size(),
            _commit_log_replay_positions_map.size());

    std::vector<index_and_replay_position> ret;
    ret.reserve(entries.size());

    // Clone replay positions for each entry. Every entry must have a
    // corresponding handle in the map — missing entries indicate a bug.
    for (const auto& entry : entries) {
        auto it = _commit_log_replay_positions_map.find(entry->idx);
        if (it == _commit_log_replay_positions_map.end()) {
            on_internal_error(logger, fmt::format("missing replay position handle for group_id={}, idx={}", _group_id, entry->idx));
        }
        ret.emplace_back(index_and_replay_position{.index = it->first, .replay_position_handle = it->second.clone()});
    }

    logger.debug("get_replay_position_handles_for completed: returned_count={}, remaining_map_size={}", ret.size(), _commit_log_replay_positions_map.size());
    return ret;
}
} // namespace service::strong_consistency
