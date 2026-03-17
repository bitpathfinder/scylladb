# ScyllaDB Process Startup Flow

This document describes the startup sequence of a ScyllaDB node: how it obtains
its configuration, initializes subsystems, joins (or rejoins) the cluster, and
becomes ready to serve client requests.

## 1. Entry Point and Tool Dispatch

**File:** `main.cc` — `main()` → `scylla_main()`

The `main()` function performs:

1. **CPU sanity check** — verifies the CPU supports required instruction sets.
2. **Tool dispatch** — the first argument selects the mode: `server` (default),
   `sstable`, `nodetool`, `types`, or perf tools. If no tool is specified or
   the argument starts with `-`, the server path (`scylla_main()`) is taken.
3. **Pre-init option handling** — `--version`, `--build-id`, `--build-mode`,
   and `--list-tools` are handled without initializing Seastar.

`scylla_main()` sets up the `app_template` with Seastar configuration defaults
and enters the reactor via `app.run()`, which starts the Seastar event loop and
calls the async lambda that drives the rest of the startup.

## 2. Configuration Loading

### 2.1 Config Object Creation

Before entering the reactor (still in `scylla_main`):

1. A `db::extensions` object is created (registers CDC, tags, rate limits, etc.).
2. A `db::config` object is created with those extensions.
3. `cfg->add_all_default_extensions()` registers built-in extensions.
4. Command-line options are added: version flags, `--options-file`,
   deprecated options, plus all options from `configurable::append_all()` and
   `cfg->add_options()`.

### 2.2 YAML File Reading

Inside the reactor (early in the `seastar::async` block):

```
read_config(opts, *cfg)
```

`read_config()` resolves the YAML config file:
- If `--options-file` is specified on the command line, that path is used.
- Otherwise, `db::config::get_conf_sub("scylla.yaml")` resolves the default
  path (typically `$SCYLLA_CONF/scylla.yaml` or `$SCYLLA_HOME/conf/scylla.yaml`).

The YAML file is parsed with **yaml-cpp**. Custom `YAML::convert<>` specializations
handle ScyllaDB-specific types (log levels, seed providers, experimental feature
enums, etc.). A callback logs warnings for deprecated options and errors for
invalid ones.

**Command-line options override YAML values** — Boost.Program_Options merges
both sources with command-line taking precedence.

### 2.3 Post-Config Setup

After config is read:

1. `configurable::init_all()` — initializes all registered configurables.
2. `cfg->setup_directories()` — sets up data/commitlog/hints directories.
3. `cfg->broadcast_to_all_shards()` — propagates config to all Seastar shards.
4. Logging settings are applied.

## 3. Pre-Service Infrastructure

The following infrastructure is set up before any database or cluster services:

| Order | Checkpoint | What it does |
|-------|-----------|-------------|
| 1 | Relabel config | Prometheus metric relabeling |
| 2 | Feature service | Loads feature flags, disabled-features config |
| 3 | Scheduling groups | Creates compaction, statement, memtable, gossip, commitlog, streaming scheduling groups |
| 4 | LSA tracker | Log-structured allocator configuration |
| 5 | Address resolution | Resolves `listen_address`, `broadcast_address`, `broadcast_rpc_address` |
| 6 | API server | HTTP API server starts listening (Prometheus, REST API) |
| 7 | Snitch | Endpoint snitch determines DC/rack placement |
| 8 | Token metadata | Initialized with host endpoint, DC/rack from snitch |
| 9 | ERM factory | Effective replication map factory |
| 10 | Directories | Data, commitlog, hints directories created and verified |
| 11 | Task manager | Background task management |
| 12 | Disk space monitor | Monitors data directory utilization |

## 4. Database and Core Services Initialization

### 4.1 Compaction and Storage Managers

- **Compaction manager** starts with scheduling groups for compaction and streaming.
- **SSTable storage manager** starts for managing SSTable I/O.

### 4.2 Service Level Controller

- Starts with default service level configuration (1000 shares).
- Scheduling groups for user workloads are created.

### 4.3 Database

```
checkpoint: "starting database"
db.start(cfg, dbcfg, mm_notifier, feature_service, token_metadata, cm, sstm, ...)
```

- Creates the `replica::database` on all shards.
- Commitlog is initialized on shard 0 first (to enumerate pre-existing segments
  before other shards create reserve segments).
- Per-shard database core is started.

### 4.4 Storage Proxy

```
checkpoint: "starting storage proxy"
```

- Configures SMP service groups for read/write/hints/acks.
- Creates `storage_proxy` on all shards (does not yet start RPC verbs).

### 4.5 Query Processor

```
checkpoint: "starting query processor"
```

- Starts on all shards with references to proxy, data dictionary, migration
  notifier, and lang manager (for UDFs).

## 5. System Keyspace and Schema Loading

### 5.1 System Keyspace Initialization

```
checkpoint: "starting system keyspace"
checkpoint: "loading system sstables"
```

1. `sys_ks.start()` — creates system keyspace service on all shards.
2. `distributed_loader::init_system_keyspace()` — loads SSTables for all system
   column families. This is done only by shard 0 to avoid races.
3. `feature_service.on_system_tables_loaded()` — re-enables previously enabled
   cluster features from `system.scylla_local`. This must happen before
   commitlog replay since features can affect storage format.

### 5.2 Upgrade Checks

For a bootstrapped node, the system verifies:
- Raft group0 upgrade is complete (`use_post_raft_procedures`).
- Raft topology upgrade is `done`.
- Auth version is `v2`.
- Service levels version is `2`.

If any of these checks fail, startup is aborted — downgrade paths are not
supported.

### 5.3 Host ID

`initialize_local_info_thread()` loads or generates the node's `host_id`:
- If a host ID exists in `system.local`, it is reused.
- Otherwise, a random UUID is generated and persisted.

### 5.4 Schema Commitlog Replay

```
checkpoint: "replaying schema commit log"
```

Schema commitlog is replayed **before** the data commitlog. This ensures the
schema is consistent before data mutations are applied:

1. Get list of schema commitlog segments to replay.
2. `commitlog_replayer::create_replayer()` creates a replayer.
3. `rp.recover(paths, COMMITLOG_FILENAME_PREFIX)` replays mutations.
4. Flush all memtables (schema commitlog uses null sharder — only shard 0).
5. Delete replayed segments.

## 6. Messaging, Gossip, and Raft Setup

### 6.1 Messaging Service

```
checkpoint: "starting messaging service"
```

The messaging service is created with:
- Host ID, listen/broadcast addresses, ports.
- TLS/SSL configuration (if `internode_encryption` is set).
- Compression settings.

**Key design point:** `messaging.start()` initializes TLS structures but does
**NOT** start listening yet. Listening is delayed until after gossip handlers
and group0 are set up, to avoid receiving RPC messages before the node is ready
to handle them.

### 6.2 Gossip

```
checkpoint: "starting gossiper"
```

**Seed parsing** (`init.cc` — `get_seeds_from_db_config()`):
- Seeds are read from the `seed_provider` config section in YAML.
- The `seeds` parameter is a comma-separated list of addresses.
- Each address is DNS-resolved. Lookup failures cause a fatal error
  for nodes that haven't bootstrapped yet.
- If no seeds are configured, defaults to `127.0.0.1`.

The gossiper is created on all shards with:
- Seeds, cluster name, partitioner.
- `ring_delay_ms`, `shadow_round_ms`, `shutdown_announce_ms`.
- The group0 ID (if previously persisted).
- Failure detector timeout.

### 6.3 Failure Detector

- **Direct failure detector pinger** — sends ping RPCs.
- **Direct failure detector** — tracks node liveness with configurable
  ping interval and timeout.

### 6.4 Raft Group Registry

```
checkpoint: "starting Raft Group"
```

- `raft_gr.start(host_id, messaging, failure_detector)` — creates the Raft
  group registry.
- `group0_client` is created (shard 0 only) — provides the interface for
  submitting group0 commands.
- `group0_service` is created — manages group0 lifecycle.

## 7. Pre-Join Service Setup

Before `join_cluster()` is called, many services are started. They are
created in dependency order:

| Checkpoint | Service | Key Dependencies |
|-----------|---------|-----------------|
| "starting tablet allocator" | Tablet allocator | mm_notifier, db |
| "starting mapreduce service" | Mapreduce (aggregate queries) | messaging, proxy, db |
| "starting migration manager" | Migration manager | mm_notifier, feature_service, messaging, proxy, gossiper, group0_client |
| "starting system distributed keyspace" | `system_distributed_keyspace` | qp, mm, proxy |
| "starting view update generator" | View updates | db, proxy |
| "starting the view builder" | View builder | db, sys_ks, sys_dist_ks, group0_client |
| "starting repair service" | Repair | gossiper, messaging, db, proxy |
| "starting streaming service" | Stream manager | db, messaging, gossiper |
| "starting auth cache" | Auth cache | qp |
| "initializing strongly consistent groups manager" | SC groups manager | messaging, raft_gr, qp, db, mm |
| "initializing storage service" | Storage service | All of the above |
| "initializing query processor remote part" | QP remote | mm, mapreduce, ss, group0_client |
| "initializing virtual tables" | Virtual tables | db, ss, gossiper, raft_gr |

### Loading Non-System Data

```
checkpoint: "loading tablet metadata"
checkpoint: "loading non-system sstables"
```

- Tablet metadata is loaded from system tables into `token_metadata`.
- Non-system keyspace SSTables are loaded.
- Migration manager feature listeners are registered.

### Data Commitlog Replay

```
checkpoint: "replaying commit log"
```

After non-system SSTables are loaded:

1. Get list of data commitlog segments.
2. Create replayer and replay mutations.
3. Flush all memtables on all shards.
4. Delete replayed segments (distributed across shards for parallelism).
5. Drop stale truncation replay position records.
6. Enable auto-compaction on all tables.

### Final Pre-Join Setup

```
checkpoint: "initializing storage proxy RPC verbs"
checkpoint: "starting CDC Generation Management service"
checkpoint: "starting CDC log service"
checkpoint: "starting storage service"
```

- Storage proxy RPC verbs registered (read/write/mutation forwarding).
- CDC generation and log services started.
- Storage service registered as gossip subscriber and migration listener.

## 8. Group0 Setup (Before Networking)

This is a critical section — it happens **before** the messaging service starts
accepting connections.

### 8.1 Group0 Service Start

```
checkpoint: "starting group 0 service"
```

- `group0_service.start()` initializes the group0 service.
- `ss.local().set_group0(group0_service)` — links storage service to group0.
- `ss.local().init_address_map()` — loads peer addresses from `system.peers`.

### 8.2 Loading Persisted Group0 State

```
group0_service.setup_group0_if_exist(sys_ks, ss, qp, mm)
```

If the node was previously part of a group0 (i.e., `system.raft_group0_id`
exists):

1. Loads the Raft server state from `system.raft`, `system.raft_snapshots`,
   and `system.raft_snapshot_config`.
2. Starts the group0 Raft server with persisted state.
3. Applies locally persisted group0 snapshots — this ensures the node's
   in-memory state (schema, topology) is at least as recent as what was
   persisted locally.

**Why before networking:** After this call, tablet Raft groups are started
using the latest locally-available tablet metadata. The
`groups_manager.start()` call waits for all these Raft groups to be ready.
Only then do we enable messaging — ensuring the node can correctly handle
proxied requests from other replicas.

### 8.3 Enable Messaging

```
messaging.start_listen(token_metadata, host_id_resolver)
```

The messaging service now starts accepting incoming RPC connections. At this
point:
- Gossip message handlers are registered.
- Group0 Raft server is running.
- Tablet Raft groups are initialized.

## 9. Joining the Cluster (`join_cluster`)

```
checkpoint: "join cluster"
```

This is the main event of startup — the node becomes part of the cluster.
`stop_signal.ready(true)` allows Ctrl+C to abort during this potentially
long operation.

### 9.1 Pre-Join Checks

1. **Decommission check** — if the node was previously decommissioned, startup
   fails.
2. Set mode to `STARTING`.
3. Load persisted endpoint state and peer features from system tables.
4. Determine initial contact nodes:
   - First startup: use seeds from config.
   - Rejoin: use previously known peers.

### 9.2 Group0 Discovery

If the node is not already part of group0:

1. **`discover_group0()`** — contacts initial nodes to find group0:
   - Creates a `persistent_discovery` object with seed list.
   - Runs peer exchange protocol: contacts seeds via RPC, learns group0 ID.
   - If elected as discovery leader and this is the first node in the cluster:
     bootstraps group0 with itself as the sole voter.
   - Otherwise, joins existing group0 via a handshaker.

2. **`join_group0()`** — joins the Raft group:
   - If discovery elected this node as leader: bootstraps group0, sets initial
     topology.
   - Otherwise: creates a Raft server, starts it, and coordinates with existing
     members via `post_server_start()` handshake.
   - Persists `system.raft_group0_id`.

### 9.3 Gossip Shadow Round

For a restarting node, a **shadow round** is performed:
- The node sends empty gossip digests to seeds/known peers.
- It collects cluster state without announcing itself.
- Exits when it receives a full state ACK from a non-shadow node, or all
  seeds are also in shadow round.

### 9.4 Start Gossiping

```
gossiper.start_gossiping(generation_number, application_states)
```

Application states include: host ID, RPC address, tokens, release version,
supported features, DC/rack. The node is now **visible to the cluster** via
gossip.

### 9.5 Group0 Setup (for joiners)

```
group0->setup_group0(sys_ks, initial_contact_nodes, handshaker, ss, qp, mm, join_params)
```

For nodes that are newly joining:
- Creates a join request for the topology coordinator.
- Coordinates with existing group0 members via RPC.

### 9.6 Topology Coordinator Processing

The topology coordinator (running on the Raft leader) processes the join:

```
node_state::none → wait_for_ip → join_group0 → bootstrapping → normal
```

1. **Validation** — feature compatibility, cluster state checks.
2. **wait_for_ip** — waits for gossiper to learn the joining node's IP.
3. **join_group0** — transitions node to bootstrapping state.
4. **bootstrapping** — streams data/tablets from existing nodes.
5. **normal** — node enters `NORMAL` state.

The joining node calls `wait_for_topology_request_completion(request_id)` and
blocks until the topology coordinator confirms the transition to `NORMAL`.

### 9.7 Start Topology Monitoring

After joining:
- `_raft_state_monitor` fiber starts — reacts to topology changes from Raft.
- `_sstable_vnodes_cleanup_fiber` — manages old vnode cleanup.

### 9.8 Set NORMAL Mode

The node enters `NORMAL` mode — it is now a fully operational member of the
cluster.

## 10. Group0 State Catch-Up Mechanism

### 10.1 Group0 State Machine

The group0 state machine (`group0_state_machine`) is the central component
that applies cluster-wide changes via Raft consensus. It runs only on shard 0.

### 10.2 Command Types

| Command Type | What It Contains | How It's Applied |
|-------------|-----------------|-----------------|
| `schema_change` | Mutations to schema tables | `migration_manager::merge_schema_from()` |
| `topology_change` | Mutations to topology/tablets tables | `write_mutations_to_database()` via storage_proxy |
| `mixed_change` | Both schema and topology mutations | Both methods, in sequence |
| `write_mutations` | Mutations to other system tables | `write_mutations_to_database()` |
| `broadcast_table_query` | Broadcast table operations | `broadcast_tables::execute_broadcast_table_query()` |

### 10.3 Applying Log Entries

When the Raft state machine's `apply()` is called:

1. Each log entry is deserialized as a `group0_command`.
2. **State ID validation**: each command carries a `prev_state_id` and
   `new_state_id`. If the `prev_state_id` doesn't match the current state,
   the command is a no-op (concurrent modification detected — the conflicting
   command was already rejected at the CQL level and the client got an error).
3. Commands are batched and applied via `merge_and_apply()`.

### 10.4 In-Memory State Reload

After applying mutations, specific tables trigger in-memory cache updates:

| Table | Action |
|-------|--------|
| `service_levels_v2` | `update_service_levels_cache()` |
| `role_members` / `role_attributes` | `auth_cache.load_roles()` |
| `dicts` | `compression_dictionary_updated_callback()` |
| `view_building_tasks` / `view_build_status_v2` | `view_building_transition()` |
| `cdc_streams_state` / `cdc_streams_history` | `load_cdc_streams()` |

### 10.5 Catch-Up on Restart

When a node restarts:

1. **Schema loaded from disk** — during database initialization, schema tables
   in `system_schema.*` are read and in-memory schema trees are populated.
2. **Group0 catches up** — after `setup_group0_if_exist()`, any locally
   persisted Raft snapshots/log entries are loaded and applied.
3. **After join** — `enable_in_memory_state_machine()` triggers `reload_state()`
   which reloads all group0-managed state into memory:
   - `topology_state_load()` — loads topology from disk into `token_metadata`.
   - `view_building_state_load()` — loads view building state.
   - `update_service_levels_cache()` — loads service levels.
4. **Raft log replay** — any log entries committed while the node was down are
   delivered by the Raft protocol and applied through the state machine.

## 11. Post-Join Initialization

After `join_cluster()` returns, the node completes its initialization:

### 11.1 Group0-Dependent Services

| Checkpoint | Service | Why it needs group0 |
|-----------|---------|-------------------|
| "starting audit service" | Audit | Needs to create keyspace/table |
| "starting tracing" | Tracing | Uses query processor with group0 |
| Service level reload | Service level controller | Reads SL data from group0 tables |
| View builder virtual table | View builder | Needs `system_distributed` keyspace |
| "starting auth service" | Authentication/Authorization | Auth version read from group0 `scylla_local` |

### 11.2 Auth Service

Auth service starts only after the Raft leader is elected and
`auth_version` is written to `scylla_local`. It initializes:
- Authorizer (default: `AllowAllAuthorizer`)
- Authenticator (default: `AllowAllAuthenticator`)
- Role manager (default: `CassandraRoleManager`)

### 11.3 Background Services

| Checkpoint | Service |
|-----------|---------|
| "starting batchlog manager" | Replays uncommitted batches |
| "starting load meter" | Tracks per-node data load |
| "starting cf cache hit rate calculator" | Cache efficiency metrics |
| "starting view update backlog broker" | MV write backlog management |
| "allow replaying hints" | Enables hinted handoff replay |
| "starting view builders" | Materialized view building |
| "starting the expiration service" | TTL / row expiration |

### 11.4 Client-Facing Transport

```
cql_transport::controller cql_server_ctl(...)
alternator::controller alternator_ctl(...)
```

- CQL native transport server is created and registered.
- Alternator (DynamoDB-compatible API) server is created if configured.
- `auth_service.ensure_superuser_is_created()` ensures the default superuser
  exists.
- Servers are started via `register_protocol_server()`.

### 11.5 Ready

```
stop_signal.ready()
supervisor::notify("serving")
```

The node logs "initialization completed" and is now serving client requests.
It blocks on `stop_signal.wait()` until SIGINT/SIGTERM is received.

## 12. Shutdown

On receiving SIGINT/SIGTERM:

1. `ss.local().drain_on_shutdown()` — flushes memtables, stops compactions,
   stops accepting new requests.
2. All `defer_verbose_shutdown()` destructors execute in reverse order of
   creation — stopping services cleanly.
3. The process exits via `_exit(0)`.

## Appendix: Initialization Dependency Graph

```
Configuration (YAML + CLI)
    │
    ├── Feature Service
    ├── Scheduling Groups
    ├── Snitch (DC/rack)
    └── Token Metadata
            │
            ├── Database
            │   ├── Commitlog (shard 0 first)
            │   └── Compaction Manager
            │
            ├── Storage Proxy
            └── Query Processor
                    │
                    └── System Keyspace
                        ├── Schema Commitlog Replay
                        ├── Host ID (load or generate)
                        └── Upgrade State Checks
                                │
                                ├── Messaging Service (create, NOT listening)
                                ├── Gossiper (create, NOT gossiping)
                                ├── Failure Detector
                                └── Raft Group Registry
                                    ├── Group0 Client
                                    ├── Group0 Service
                                    └── SC Groups Manager
                                            │
                                            ├── Migration Manager
                                            ├── Storage Service
                                            ├── Tablet Metadata Load
                                            ├── Non-System SSTable Load
                                            ├── Data Commitlog Replay
                                            └── CDC Services
                                                    │
                                                    ├── group0_service.start()
                                                    ├── setup_group0_if_exist()
                                                    ├── groups_manager.start()
                                                    └── messaging.start_listen() ← NOW accepting RPCs
                                                            │
                                                            └── join_cluster()
                                                                ├── Gossip Shadow Round
                                                                ├── discover_group0()
                                                                ├── start_gossiping()
                                                                ├── setup_group0() / join_group0()
                                                                ├── Topology Coordinator processes join
                                                                └── Node becomes NORMAL
                                                                        │
                                                                        ├── Auth Service
                                                                        ├── Service Levels
                                                                        ├── View Builders
                                                                        ├── Hint Replay
                                                                        ├── Expiration Service
                                                                        └── CQL / Alternator Servers
                                                                                │
                                                                                └── SERVING
```

## Appendix: Key Source Files

| File | Role |
|------|------|
| `main.cc` | Entry point, orchestrates entire startup and shutdown |
| `init.cc` / `init.hh` | Seed parsing, helper initialization functions |
| `db/config.hh` / `db/config.cc` | Configuration object, YAML parsing |
| `service/storage_service.cc` | `join_cluster()`, topology state transitions |
| `service/raft/raft_group0.cc` | Group0 discovery, join, setup, state management |
| `service/raft/raft_group0.hh` | Group0 service interface |
| `service/raft/group0_state_machine.cc` | Raft state machine — applies log entries |
| `service/topology_coordinator.cc` | Processes topology changes (joins, leaves, etc.) |
| `service/topology_state_machine.hh` | Topology state machine definition |
| `gms/gossiper.cc` | Gossip protocol implementation |
| `raft/server.cc` | Core Raft server — loads persisted state on startup |
| `db/system_keyspace.cc` | System table definitions and operations |
| `db/commitlog/commitlog_replayer.cc` | Commitlog replay logic |

