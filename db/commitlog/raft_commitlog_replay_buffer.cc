/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#include "replica/database.hh"
#include "db/commitlog/raft_commitlog_replay_buffer.hh"
#include "db/commitlog/commitlog.hh"
#include "db/commitlog/commitlog_entry.hh"
#include "db/commitlog/commitlog_entry_io.hh"
#include "db/timeout_clock.hh"
#include "cql3/query_processor.hh"
#include "service/strong_consistency/raft_groups_storage.hh"
#include "mutation/frozen_mutation.hh"
#include "schema/schema_registry.hh"
#include "db/system_keyspace.hh"
#include "service/strong_consistency/state_machine.hh"
#include "serializer_impl.hh"
#include "idl/strong_consistency/state_machine.dist.hh"
#include "idl/strong_consistency/state_machine.dist.impl.hh"
#include "idl/commitlog.dist.hh"
#include "idl/commitlog.dist.impl.hh"
#include "idl/raft_storage.dist.hh"
#include "idl/raft_storage.dist.impl.hh"

#include <algorithm>
#include <seastar/core/coroutine.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/coroutine/maybe_yield.hh>

namespace db {

static seastar::logger logger("raft_commitlog_replay");

namespace {
// Build a mapping from group_id to table_id using tablet metadata.
std::unordered_map<raft::group_id, table_id> build_group_to_table_map(const locator::token_metadata& tm) {
    std::unordered_map<raft::group_id, table_id> result;
    const auto& tablets = tm.tablets();
    for (const auto& [tid, _] : tablets.all_table_groups()) {
        const auto& tablet_map = tablets.get_tablet_map(tid);
        if (!tablet_map.has_raft_info()) {
            continue;
        }
        for (const auto& tablet_id : tablet_map.tablet_ids()) {
            const auto gid = tablet_map.get_tablet_raft_info(tablet_id).group_id;
            result.emplace(gid, tid);
        }
    }
    return result;
}

struct filter_entries_result {
    std::vector<raft::log_entry_ptr> entries;
    uint64_t discarded_leader_change = 0;
    uint64_t skipped_duplicates = 0;
    uint64_t skipped_old_term = 0;
};

// First pass: Filter entries by handling leader changes, duplicates, and superseded terms.
// This function does not perform any database or commitlog writes.
// Returns the filtered entries along with discard statistics.
//
// Handles crash recovery scenarios where both old and new commitlog segments are replayed:
// - Tracks the maximum term seen to skip entries from superseded terms
// - Detects consecutive duplicates (same idx+term as previous entry)
// - Handles leader changes by discarding entries with idx >= new entry's idx
//
// Note: This function does NOT filter by snapshot_idx. All entries pass through so that
// the caller can apply committed entries to memtables even if they are already snapshotted.
// After a crash, snapshotted entries may not have been flushed to sstables, so they must
// be re-applied during replay.
filter_entries_result filter_entries(utils::chunked_vector<raft::log_entry_ptr>& entries_list, raft::group_id group_id) {
    filter_entries_result result;

    raft::index_t last_idx{0};
    raft::term_t last_term{0};
    raft::term_t max_term{0}; // Track the highest term we've seen

    for (auto& entry : entries_list) {
        // Skip entries from terms that have been superseded by a higher term.
        // This handles crash recovery where we replay old segments containing
        // entries from terms that were already replaced by a leader change.
        if (entry->term < max_term) {
            logger.debug("group {}: skipping entry from old term {} (max_term={}), idx={}", group_id, entry->term, max_term, entry->idx);
            ++result.skipped_old_term;
            continue;
        }

        // Update max_term if we see a higher term
        if (entry->term > max_term) {
            max_term = entry->term;
        }

        // Check for out-of-order indices which indicate either a leader change
        // (term changed) or a duplicate tail from an older segment (term unchanged).
        auto ordering = check_entry_ordering(entry->idx, entry->term, last_idx, last_term);

        switch (ordering) {
        case entry_ordering_check_result::leader_change: {
            // Leader changed — discard all entries with idx >= entry->idx from
            // the previous term. These were uncommitted entries from the old leader
            // that were replaced by the new leader.
            //
            // Since entries are sorted by idx, we can use binary search to find
            // the first entry with idx >= entry->idx and erase from there to the end.
            auto it = std::ranges::lower_bound(result.entries, entry->idx, {}, [](const raft::log_entry_ptr& e) {
                return e->idx;
            });
            auto removed_count = std::distance(it, result.entries.end());
            result.entries.erase(it, result.entries.end());
            result.discarded_leader_change += removed_count;

            logger.info("group {}: leader change detected at idx={} term={} (previous term={}), "
                        "discarded {} entries with idx >= {}",
                    group_id, entry->idx, entry->term, last_term, removed_count, entry->idx);
            break;
        }
        case entry_ordering_check_result::duplicate:
            // Duplicate entry (same idx and same term) — skip it.
            // This can occur after crash recovery when both old and new commitlog
            // segments are replayed, causing the same entry to appear twice.
            logger.debug("group {}: skipping duplicate entry at idx={} term={}", group_id, entry->idx, entry->term);
            ++result.skipped_duplicates;
            continue;
        case entry_ordering_check_result::out_of_order_same_term:
            // Same term but smaller index — we hit the start of a duplicate
            // range from an older commitlog segment.  This happens after a crash
            // when uncommitted entries were re-written to a new segment and then
            // the old segment is replayed first.  All remaining entries from this
            // point are duplicates, so stop processing.
            logger.info("group {}: detected duplicate tail from older segment at idx={} term={} "
                        "(last_idx={}), stopping entry processing",
                    group_id, entry->idx, entry->term, last_idx);
            goto done;
        case entry_ordering_check_result::in_order:
            break;
        }

        last_idx = entry->idx;
        last_term = entry->term;

        result.entries.push_back(std::move(entry));
    }

done:
    return result;
}

} // anonymous namespace

future<> raft_commitlog_replay_buffer::process_raft_replayed_items(replica::database& db, cql3::query_processor& qp, db::system_keyspace& sys_ks) {
    if (remaining_groups() == 0) {
        co_return;
    }

    const auto token_metadata = db.get_shared_token_metadata().get();
    const auto group_to_table = build_group_to_table_map(*token_metadata);

    auto* new_commitlog_ptr = db.commitlog();
    SCYLLA_ASSERT(new_commitlog_ptr);

    logger.info("processing {} raft groups with {} total entries from commitlog replay", remaining_groups(), total_entries());

    for (auto& [group_id, entries_list] : _replayed_commitlog_entries_by_group) {
        if (entries_list.empty()) {
            continue;
        }

        // Look up table_id for this group.
        auto table_it = group_to_table.find(group_id);
        if (table_it == group_to_table.end()) {
            // Group not found in tablet metadata — the tablet may have been moved away.
            // Discard these entries since this shard no longer owns the tablet.
            logger.info("group {} not found in tablet metadata, discarding {} entries", group_id, entries_list.size());
            continue;
        }
        const auto table_id = table_it->second;

        // Query commit_idx and snapshot descriptor from raft system tables.
        const auto commit_idx = co_await service::strong_consistency::raft_groups_storage::load_commit_idx(qp, group_id, this_shard_id());
        auto snapshot = co_await service::strong_consistency::raft_groups_storage::load_snapshot_descriptor(qp, group_id, this_shard_id());

        logger.info("group {}: {} entries, commit_idx={}, snapshot_idx={}", group_id, entries_list.size(), commit_idx, snapshot.idx);

        // First pass: Filter entries by handling leader changes, duplicates, and superseded terms.
        // This must be done before any database writes to avoid applying entries that would
        // later be discarded due to leader changes.
        auto filtered = filter_entries(entries_list, group_id);

        // Second pass: Apply committed entries to database and rewrite uncommitted entries to commitlog.
        uint64_t applied = 0;
        uint64_t rewritten = 0;
        auto& group_data = _per_group_data[group_id];

        for (auto& entry : filtered.entries) {
            // Apply committed command entries to the memtables. It is safe not to append them
            // to the new commitlog, because the old commitlog (currently being replayed) will
            // only be deleted after the memtables are flushed. Therefore, the data will either
            // be persisted to SSTables or, in case of a crash, still be available in the old commitlog.
            if (entry->idx <= commit_idx && std::holds_alternative<raft::command>(entry->data)) {
                utils::chunked_vector<frozen_mutation> muts;
                muts.emplace_back(service::strong_consistency::detail::deserialize_to_frozen_mutation(entry));
                // Resolve schema and upgrade mutation if needed (no barrier during replay).
                auto schemas = co_await service::strong_consistency::resolve_and_upgrade_mutations(muts, table_id, db, sys_ks);
                co_await db.apply_in_memory(muts[0], schemas[0], db::rp_handle(), db::no_timeout);
                ++applied;
            }

            // Rewrite uncommitted entries to the new commitlog to obtain rp_handles
            // that ensure the commitlog segments won't be deleted.
            if (entry->idx > commit_idx) {
                commitlog_raft_log_entry_writer writer(raft_commit_log_entry{.group_id = group_id, .entry = entry});
                const auto write_fn = [&writer](auto& out) {
                    return writer.write(out);
                };
                const auto target_size = writer.size();
                auto handle = co_await new_commitlog_ptr->add(table_id, target_size, db::no_timeout, db::commitlog_force_sync::yes, write_fn);
                group_data.replay_positions.emplace(entry->idx, std::move(handle));
                ++rewritten;
                co_await seastar::coroutine::maybe_yield();
            }

            // Only add entries after the snapshot to the raft log. Snapshotted entries
            // don't need to be in the log for replication — their effects are captured
            // in the snapshot. But they were still applied above if committed, because
            // after a crash the snapshot data may not have been flushed to sstables.
            if (entry->idx > snapshot.idx) {
                group_data.entries.push_back(std::move(entry));
            }
        }

        logger.info("group {}: discarded_leader_change={}, skipped_duplicates={}, skipped_old_term={}, applied={}, rewritten={}, "
                    "total_in_log={}",
                group_id, filtered.discarded_leader_change, filtered.skipped_duplicates, filtered.skipped_old_term, applied, rewritten,
                group_data.entries.size());
    }

    // The old items are not needed anymore.
    _replayed_commitlog_entries_by_group.clear();
    logger.info("Raft groups commit log replayed data processing complete");
}

} // namespace db
