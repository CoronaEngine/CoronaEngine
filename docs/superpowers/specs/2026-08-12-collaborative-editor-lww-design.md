# Collaborative Editor State Synchronization Design

## Scope

This design covers collaborative editor state only. It excludes runtime game
input, physics, frame synchronization, prediction, rollback, and lockstep.

The system must converge on the same editor scene state across LAN peers for
Actor lifecycle and editable properties, including after a peer joins or
reconnects.

## Design Decisions

The synchronization model remains full-mesh over ENet, but state conflicts are
resolved with field-level Last-Write-Wins (LWW). Every replicated field is
identified by a stable Actor GUID and field name, never by a process-local
storage sequence.

Each update carries:

```text
actor_guid
field_name
value
version = (lamport_counter, writer_peer_id)
```

Versions compare lexicographically: the larger Lamport counter wins; equal
counters are resolved by the larger writer peer ID. Equal versions are
duplicates and are ignored.

The local Lamport counter is advanced on every local edit and on every received
message:

```text
local_counter = max(local_counter, received_counter) + 1   // local edit
local_counter = max(local_counter, received_counter)       // receive
```

`steady_clock` remains available for transport timeouts only. It is not used
for cross-peer conflict ordering.

## Replicated State

The logical state layer contains:

- Actor creation metadata and scene membership;
- Actor deletion tombstones;
- Transform fields;
- Environment fields;
- Optics/material editor fields;
- Logical resource identifiers and paths needed to rebuild local resources.

GPU handles, storage handles, pointers, and other process-local objects are
never serialized. Geometry and model resources are reconstructed locally after
their logical resource identity and required files are available.

Actor create, field update, and delete all use the same version model:

```text
UPSERT(actor_guid, field, value, version)
DELETE(actor_guid, version)
SNAPSHOT(entries...)
```

Delete creates a tombstone. Updates at or below the tombstone version are
rejected, so an old packet cannot resurrect a deleted Actor. Tombstones may be
garbage-collected only after a defined snapshot/peer-acknowledgement policy is
implemented.

## Data Flow

### Local edit

1. The editor changes a logical field.
2. The state layer increments its Lamport counter and creates a version using
   the local stable peer ID.
3. The field value and version are applied locally.
4. An idempotent `UPSERT` or `DELETE` operation is queued for reliable
   broadcast.

### Remote update

1. Decode and validate the operation.
2. Advance the local Lamport counter to at least the received counter.
3. Resolve `actor_guid` to the local storage object, if it exists.
4. Compare the incoming version with the stored field/tombstone version.
5. Apply only if the incoming version is newer; otherwise ignore it as stale or
   duplicate.
6. If a write lock is temporarily unavailable, retain the operation for retry;
   never silently drop it.

### New peer and reconnect

```text
HELLO -> peer ready -> request/send SNAPSHOT -> apply per-field LWW
       -> resume incremental UPSERT/DELETE operations
```

Snapshots are idempotent and are merged per field using the same version rules;
they must not blindly overwrite newer local edits. Reconnection always performs
the snapshot exchange because dirty polling alone cannot recover updates missed
during a disconnect.

## Protocol Requirements

The protocol must carry a stable protocol version and explicit bounds for:

- Actor GUID length;
- Field name length;
- Value length;
- Operation count;
- Snapshot size/chunking.

Malformed packets, unknown storage/field identifiers, invalid versions, and
oversized payloads are rejected without mutating state. Runtime snapshots and
editor synchronization use separate message types and queues; editor sync is
not reused for frame-rate runtime state.

## Peer Management

The existing HELLO/rekey flow remains, but duplicate connection cleanup must be
deterministic and explicit: save the old `ENetPeer*`, remove the old list entry,
and disconnect the old ENet connection before retaining the selected link.

Heartbeat send, last-received tracking, and application timeout handling are
required. After a timeout or ENet disconnect, the next successful connection
must trigger the snapshot exchange.

## Testing and Acceptance

The implementation is accepted only when tests demonstrate:

- Concurrent edits of one field converge deterministically by LWW;
- Different fields merge independently;
- Stale and duplicate operations are idempotently ignored;
- Delete tombstones prevent resurrection by old updates;
- A new peer receives a complete scene snapshot;
- Reconnect restores changes made while disconnected;
- Snapshot and incremental operations can arrive in either order;
- Temporary write-lock contention results in eventual application;
- Optics and other declared editor fields synchronize;
- Malformed/oversized packets do not mutate state;
- Simultaneous cross-connect leaves exactly one active peer link.

The implementation must not claim runtime input, physics, or frame-level
synchronization support as part of this milestone.

## Out of Scope Follow-ups

Runtime server authority, client prediction, rollback, deterministic physics,
input replication, and Internet NAT traversal require a separate design.
