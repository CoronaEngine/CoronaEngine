# Collaborative Editor LWW Implementation Plan

Design reference: `docs/superpowers/specs/2026-08-12-collaborative-editor-lww-design.md`

## Phase 0: Baseline and compatibility

Files: `include/corona/systems/network/protocol.h`, network protocol tests.

- Introduce a new editor-sync protocol version/message schema rather than
  silently changing the existing binary layout.
- Add bounded readers/writers for Actor GUID, field name, value, Lamport
  counter, writer peer ID, operation count, and snapshot chunks.
- Define explicit operation kinds: UPSERT, DELETE, SNAPSHOT_BEGIN,
  SNAPSHOT_CHUNK, SNAPSHOT_END, and SNAPSHOT_REQUEST.
- Add encode/decode tests for valid, duplicate, truncated, unknown, and
  oversized packets.

Exit criterion: malformed packets are rejected without reading past the
buffer, and old packet formats cannot be mistaken for the new editor-sync
messages.

## Phase 1: LWW state core

Files: new `include/corona/systems/network/lww_state.h` and source/tests, or
an equivalent focused module following local conventions.

- Define `LwwVersion { uint64_t counter; string writer_peer_id; }` and a single
  deterministic comparator.
- Store versions by `(actor_guid, field_name)` and deletion tombstones by
  `actor_guid`.
- Expose idempotent apply operations returning applied/stale/invalid status.
- Advance the local Lamport counter on local edits and received operations.
- Keep pending operations when the target storage write lock is unavailable;
  retry from the system update path.
- Make delete tombstones dominate all field updates at or below the delete
  version.

Exit criterion: unit tests prove convergence for concurrent edits, independent
field merges, duplicates, stale updates, delete resurrection prevention, and
lock-contention retry.

## Phase 2: SyncEngine integration

Files: `include/corona/systems/network/sync_engine.h`,
`src/systems/network/sync_engine.cpp`.

- Replace timestamp-based dirty entries with GUID/field/version operations.
- Use local entity sequence only to resolve a GUID to a current
  `SharedDataHub` object.
- Update dirty detection snapshots to be keyed by GUID and field.
- Apply incoming operations through the LWW state core; never update storage
  directly without version comparison.
- Add all in-scope editor fields, including Optics; exclude GPU and pointer
  handles.
- Remove the recursive/broken `SYNC_FULL` parsing path.

Exit criterion: SyncEngine-level tests show local and remote edits converge and
remote writes are not silently lost.

## Phase 3: Actor lifecycle unification

Files: `src/systems/network/network_system.cpp`, actor message builders and
related headers/tests.

- Give create, delete, rename, scene membership, and logical resource changes
  the same Actor GUID/version envelope as property updates.
- Keep file transfer as a transport concern; only publish the logical resource
  update after the required file is available locally.
- Make create/delete/update handlers idempotent and reject operations blocked
  by a newer tombstone/version.
- Ensure local actor registration and identity mappings are updated before
  queued property operations are applied.

Exit criterion: an Actor created, modified, or deleted on one peer reaches the
other peer exactly once logically, regardless of duplicate or reordered
messages.

## Phase 4: Full snapshot and reconnection

Files: `SyncEngine`, `NetworkSystem`, protocol tests, integration tests.

- On peer-ready callback, request or send a snapshot to that peer rather than
  broadcasting it to every peer.
- Implement snapshot begin/chunk/end with bounded chunk sizes and an explicit
  snapshot identifier.
- Merge snapshot entries through the same LWW state core.
- Queue incremental operations while a snapshot is being applied, then replay
  them by version.
- Trigger the same exchange after a disconnect/reconnect; do not rely on dirty
  polling to recover missed changes.

Exit criterion: a new or reconnected peer converges to the complete scene,
including changes made while it was offline.

## Phase 5: Peer liveness and duplicate-link correctness

Files: `include/corona/systems/network/peer_manager.h`,
`src/systems/network/peer_manager.cpp`, `NetworkSystem`.

- Send heartbeat packets at the configured interval on the unreliable channel.
- Track last received time per peer and disconnect peers exceeding the timeout.
- Fix duplicate-link cleanup by retaining the old ENet pointer before removing
  its metadata, then explicitly disconnecting the discarded link.
- Ensure peer callbacks fire once per logical peer connection/disconnection.

Exit criterion: heartbeat timeout is observable in tests, and simultaneous
cross-connect leaves exactly one active logical and ENet link.

## Phase 6: End-to-end verification and rollout

- Add a two-instance in-process or loopback ENet harness where practical.
- Test concurrent edits, packet duplication/reordering, join snapshots,
  reconnect recovery, deletion tombstones, and malformed traffic.
- Run the existing network protocol suite and the full project test target.
- Document the new protocol version and explicitly state that runtime input,
  physics, and frame synchronization remain unsupported.

Exit criterion: all acceptance cases in the design document have executable
evidence or a documented integration-test limitation.

## Dependencies and sequencing

```text
Phase 0 -> Phase 1 -> Phase 2 -> Phase 3
                              -> Phase 4
Phase 5 can proceed after protocol compatibility is fixed
Phase 6 requires Phases 0-5
```

No implementation phase should start by expanding the set of replicated
fields. The versioned state core and lifecycle semantics must be in place first
so every newly added field has the same conflict and recovery behavior.
