/*
 * Copyright 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#include "raft/raft.hh"

#include "idl/raft_storage.idl.hh"

namespace raft {

struct snapshot_descriptor {
    raft::index_t idx;
    raft::term_t term;
    raft::configuration config;
    raft::snapshot_id id;
};

struct vote_request {
    raft::term_t current_term;
    raft::index_t last_log_idx;
    raft::term_t last_log_term;
    bool is_prevote;
    bool force;
};

struct vote_reply {
    raft::term_t current_term;
    bool vote_granted;
    bool is_prevote;
};

struct install_snapshot {
    raft::term_t current_term;
    raft::snapshot_descriptor snp;
};

struct snapshot_reply {
    raft::term_t current_term;
    bool success;
};

struct append_reply {
    struct rejected {
        raft::index_t non_matching_idx;
        raft::index_t last_idx;
    };
    struct accepted {
        raft::index_t last_new_idx;
    };
    raft::term_t current_term;
    raft::index_t commit_idx;
    std::variant<raft::append_reply::rejected, raft::append_reply::accepted> result;
};

struct append_request {
    raft::term_t current_term;
    raft::index_t prev_log_idx;
    raft::term_t prev_log_term;
    raft::index_t leader_commit_idx;
    std::vector<lw_shared_ptr<const raft::log_entry>> entries;
};

struct timeout_now {
    raft::term_t current_term;
};

struct read_quorum {
    raft::term_t current_term;
    raft::index_t leader_commit_idx;
    raft::read_id id;
};

struct read_quorum_reply {
    raft::term_t current_term;
    raft::index_t commit_idx;
    raft::read_id id;
};

struct not_a_leader {
    raft::server_id leader;
};

struct not_a_member {
    sstring message();
};

struct transient_error {
    sstring message();
    raft::server_id leader;
};

struct commit_status_unknown {
};

struct entry_id {
    raft::term_t term;
    raft::index_t idx;
};
} // namespace raft

namespace service {

struct wrong_destination {
    raft::server_id reached_id;
};

struct group_liveness_info {
    bool group0_alive;
};

struct direct_fd_ping_reply {
    std::variant<std::monostate, service::wrong_destination, service::group_liveness_info> result;
};

} // namespace service
