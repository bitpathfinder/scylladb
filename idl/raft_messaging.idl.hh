/*
 * Copyright 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

namespace raft {
    verb [[with_client_info, with_timeout]] raft_send_snapshot (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::install_snapshot snap [[ref]]) -> raft::snapshot_reply;
    verb [[with_client_info, with_timeout, one_way]] raft_append_entries (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::append_request req [[ref]]);
    verb [[with_client_info, with_timeout, one_way]] raft_append_entries_reply (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::append_reply);
    verb [[with_client_info, with_timeout, one_way]] raft_vote_request (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::vote_request);
    verb [[with_client_info, with_timeout, one_way]] raft_vote_reply (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::vote_reply);
    verb [[with_client_info, with_timeout, one_way]] raft_timeout_now (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::timeout_now);
    verb [[with_client_info, with_timeout, one_way]] raft_read_quorum (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::read_quorum);
    verb [[with_client_info, with_timeout, one_way]] raft_read_quorum_reply (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::read_quorum_reply);
    verb [[with_client_info, with_timeout]] raft_execute_read_barrier_on_leader (raft::group_id, raft::server_id from_id, raft::server_id dst_id) -> raft::read_barrier_reply;
    verb [[with_client_info, with_timeout]] raft_add_entry (raft::group_id, raft::server_id from_id, raft::server_id dst_id, raft::command cmd [[ref]]) -> raft::add_entry_reply;
    verb [[with_client_info, with_timeout]] raft_modify_config (raft::group_id gid, raft::server_id from_id, raft::server_id dst_id, std::vector<raft::config_member> add [[ref]], std::vector<raft::server_id> del [[ref]]) -> raft::add_entry_reply;
}

namespace service {
    verb [[with_client_info, with_timeout, cancellable]] direct_fd_ping (raft::server_id dst_id) -> service::direct_fd_ping_reply;
}
