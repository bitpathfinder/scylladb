/*
 * Copyright 2016-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */
#pragma once
#include <optional>
#include "mutation/frozen_mutation.hh"
#include "schema/schema.hh"
#include "raft/raft.hh"


// A frozen mutation together with its optional column mapping, as stored
// in the commitlog.  This is the original (and still default) payload type
// for commitlog entries — every normal table write goes through here.
struct mutation_entry {
    std::optional<column_mapping> _mapping;
    frozen_mutation _mutation;
public:
    mutation_entry(std::optional<column_mapping> mapping, frozen_mutation&& mutation)
        : _mapping(std::move(mapping)), _mutation(std::move(mutation)) { }
    const std::optional<column_mapping>& mapping() const { return _mapping; }
    const frozen_mutation& mutation() const & { return _mutation; }
    frozen_mutation&& mutation() && { return std::move(_mutation); }
};

// A Raft log entry (command, configuration change, or dummy) together with
// its group ID.  Stored in the database commitlog when the strongly-consistent
// tables experimental feature is enabled and the segment uses the variant
// serialization format (v5).
struct raft_commit_log_entry {
    raft::group_id group_id;
    raft::log_entry_ptr entry;
};

// The on-disk envelope for variant-format commitlog segments (v5).  Each
// entry contains exactly one of the variant alternatives: a mutation_entry
// (normal table write) or a raft_commit_log_entry (Raft log entry for
// strongly-consistent tables).
//
// Legacy segments (v4 and earlier) do not use this envelope — they store
// mutation_entry data directly, read via commitlog_mutation_entry_reader.
using commitlog_entry_variant = std::variant<raft_commit_log_entry, mutation_entry>;
struct commitlog_entry {
    commitlog_entry_variant item;
};
