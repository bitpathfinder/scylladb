# How a Node Joins Group0

Group0 is the Raft consensus group that manages cluster-wide metadata (schema,
topology, auth, service levels, etc.). Every ScyllaDB node must be a member of
group0. The join protocol has several phases.

## Phase 1: Discovery

**Entry:** `raft_group0::discover_group0()` in `service/raft/raft_group0.cc`

The joining node needs to find group0 — or create it if this is the first node.

1. Loads any previously discovered peers from `system.discovery` (survives
   restarts).
2. If no cached peers, uses **seeds** from `scylla.yaml`.
3. Runs the `persistent_discovery` algorithm: sends `group0_peer_exchange` RPCs
   to all known peers in parallel.
4. Two outcomes:
   - **An existing group0 is found** — a peer responds with
     `group0_info{group0_id, leader_id, leader_ip}`.
   - **No group0 exists** — the node with the **smallest Raft server ID** among
     discoverable peers is elected **discovery leader** and creates a brand-new
     group0.

## Phase 2: Create Raft Server

**Entry:** `create_server_for_group0()` in `service/raft/raft_group0.cc`

A local Raft server is created with:

- `group0_state_machine` — applies committed log entries (schema/topology
  mutations) to local system tables.
- `raft_sys_table_storage` — persists Raft log, snapshots, and vote to
  `system.raft`, `system.raft_snapshots`, `system.raft_snapshot_config`.
- Raft RPC implementation, ticker (periodic `tick()` calls).
- Size limits derived from commitlog segment size (max command = 1/2 segment,
  snapshot threshold = 3/4 of max log size).

## Phase 3: Bootstrap the Raft Server

Two cases:

### First node in the cluster (discovery leader)

- Creates an initial Raft configuration with itself as the sole voter.
- Calls `ss.raft_initialize_discovery_leader()` which writes the initial
  topology state to `system.topology` via group0.
- Forces a **nontrivial snapshot** (snapshot index = 1), so that any node
  joining later can receive a snapshot transfer.

### Joining an existing cluster

- The `join_node_rpc_handshaker::pre_server_start()` sends a
  **`join_node_request`** RPC to the group0 leader (the topology coordinator).
  This request contains: `host_id`, `cluster_name`, `datacenter`, `rack`,
  `num_tokens`, `supported_features`, etc.
- The topology coordinator validates the request (cluster name, snitch, feature
  compatibility) and writes the join request as a topology mutation into group0
  via Raft.
- The joining node's Raft server is `bootstrap()`'d with an empty
  configuration — it is not yet a Raft voter.

## Phase 4: Topology Coordinator Processes the Join

**Entry:** `handle_node_transition()` in `service/topology_coordinator.cc`

The coordinator (running on the Raft leader) processes the node in
`node_state::none`:

1. **Validation** (`validate_joining_node()`):
   - For replace: validates the replaced node is in `normal` state.
   - Checks that all cluster-enabled features are supported by the joining node.

2. **`wait_for_ip`** — sends a global command to all nodes so gossiper learns
   the joining node's IP before proceeding.

3. If validation fails: transitions node to `node_state::left`, sends a
   rejection RPC back.

4. If validation succeeds: sets `transition_state::join_group0` and
   `node_state::bootstrapping`.

## Phase 5: `join_group0` Transition State

When the coordinator enters `transition_state::join_group0`, it calls
`finish_accepting_node()`:

1. **Adds the joining node to the Raft configuration** via
   `raft.modify_config()` — the node becomes a group0 member.
2. **Sends `join_node_response` RPC** back to the joining node with `accepted`.
3. On the joining side, `post_server_start()` unblocks — the node knows it has
   been accepted.

Then for a bootstrapping node with tokens:

- Tokens are assigned (random or from config).
- A new CDC generation is prepared.
- State transitions through: `commit_cdc_generation` → `write_both_read_old` →
  `write_both_read_new` → the node streams data → finally
  `node_state::normal`.

For a **zero-token node**: it transitions directly to `node_state::normal`.

## Phase 6: Snapshot Transfer

When the joining node's Raft server starts and connects to the leader, the
leader may transfer a **snapshot**. This happens in
`group0_state_machine::transfer_snapshot()`:

1. Pulls schema tables from the leader via `send_migration_request()`.
2. Pulls topology snapshot (topology, topology_requests, CDC generations).
3. Pulls auth and service level tables.
4. Applies all mutations locally under a mutex.
5. Calls `reload_state()` to populate in-memory caches (topology, views,
   service levels).

This is how a new node gets the full cluster metadata without replaying the
entire Raft log.

## Phase 7: Persist and Announce

Back in `join_group0()`:

- Once the node is in the Raft configuration, the loop exits.
- `sys_ks.set_raft_group0_id(group0_id)` persists the group0 ID — on next
  restart, the node skips discovery entirely.
- Gossiper is updated with the group0 ID.

## Phase 8: `finish_setup_after_join()`

After `join_cluster()` completes:

- If the node joined as a **non-voter** (which can happen with limited voter
  configurations), it calls `modify_config()` to promote itself to a voter.

## Rejoining (Restart of an Existing Member)

A previously-joined node takes a shortcut:

1. `setup_group0_if_exist()` checks `system.raft_group0_id`. If it exists, the
   node was already in group0.
2. Loads persisted Raft state from `system.raft` / `system.raft_snapshots`.
3. Starts the Raft server — it re-joins the existing Raft group automatically.
4. Applies any locally persisted snapshot, then catches up via Raft log
   replication from the leader.
5. **No discovery or handshake needed** — the node is already in the Raft
   configuration.

## Summary Flow

```
New node                          Topology Coordinator (Raft leader)
   │                                        │
   ├─ discover_group0() ──────────────────► │ (peer exchange RPCs)
   │◄── group0_info{id, leader} ───────────┤
   │                                        │
   ├─ join_node_request{host,dc,rack,...} ─►│
   │                                        ├─ validate (features, cluster name)
   │                                        ├─ write join request to group0 via Raft
   │                                        ├─ wait_for_ip (global command)
   │                                        ├─ set node_state::bootstrapping
   │                                        ├─ enter transition_state::join_group0
   │                                        ├─ raft.modify_config(add joining node)
   │◄── join_node_response{accepted} ──────┤
   │                                        │
   ├─ Raft snapshot transfer ◄─────────────┤ (schema + topology + auth)
   │                                        │
   ├─ Raft log catch-up ◄─────────────────┤
   │                                        │
   │                                        ├─ assign tokens, CDC gen
   │                                        ├─ stream data to new node
   │                                        ├─ set node_state::normal
   │                                        │
   ├─ persist group0_id locally             │
   ├─ finish_setup_after_join()             │
   └─ DONE                                  │
```

## Key Source Files

| File | Role |
|------|------|
| `service/raft/raft_group0.cc` | Discovery, `join_group0()`, server creation, `finish_setup_after_join()` |
| `service/raft/raft_group0.hh` | Group0 service interface, `persistent_discovery` |
| `service/raft/group0_state_machine.cc` | State machine `apply()`, snapshot transfer |
| `service/storage_service.cc` | `join_cluster()`, `join_node_rpc_handshaker` |
| `service/topology_coordinator.cc` | Processes join requests, validation, state transitions |
| `service/topology_state_machine.hh` | Topology state machine definition |
| `service/raft/raft_sys_table_storage.cc` | Raft log/snapshot persistence to system tables |

