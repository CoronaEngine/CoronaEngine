# ENet Channel Scheduling Design

## Scope

Reduce editor stalls caused by reliable bulk traffic while retaining ENet Reliable UDP. This change covers transport channel assignment and local send scheduling; it does not change the editor LWW model or add runtime input/physics synchronization.

## Channel contract

Each ENet host is created with four channels:

- Channel 0, `Control`: HELLO, editor operations (`EDITOR_SYNC`), snapshot request/begin/end, actor control messages, ownership, and chat control.
- Channel 1, `Transform`: actor transform updates. These remain reliable, but the producer coalesces repeated updates for the same actor before transmission.
- Channel 2, `Bulk`: editor snapshot chunks, file requests/chunks, and chat history snapshots.
- Channel 3, `Realtime`: heartbeat and other explicitly lossy keep-alive traffic.

Existing message bytes and protocol version remain unchanged. Channel selection is transport metadata, so peers with this build must use four ENet channels.

## Scheduling

`PeerManager` exposes a per-tick bulk budget. Control and transform sends are immediate. Bulk sends are queued and drained during `poll()` up to a fixed byte/packet budget, preserving order within the bulk queue. The budget is applied per peer and resets each poll tick. This prevents a large file or snapshot from monopolizing the system update loop.

Transform coalescing is keyed by `(peer, actor_guid)` for broadcasts and keeps only the newest packet. A later control operation does not get reordered behind an older transform because transforms use a separate ENet channel.

## Reliability and failure handling

Control, transform, and bulk messages use ENet reliable delivery. Realtime messages use unreliable delivery. Queue overflow rejects only new bulk work and logs the drop; control/state changes are never dropped by the local scheduler. Existing application-level snapshot/file retry behavior remains authoritative.

## Verification

Add focused tests for four-channel host creation, message-to-channel routing, transform coalescing, and bulk budget draining. Keep the existing two-peer native loopback test and full network/editor regressions as integration gates.
