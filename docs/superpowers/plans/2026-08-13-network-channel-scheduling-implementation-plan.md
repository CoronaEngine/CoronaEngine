# ENet Channel Scheduling Implementation Plan

1. Add protocol channel constants and update ENet host/connect channel counts.
2. Add a focused PeerManager scheduling test harness or observable send hook, then make routing and bulk budget behavior fail.
3. Implement channel-aware send helpers, bulk queue draining, and transform coalescing.
4. Route NetworkSystem and SyncEngine packet classes to Control, Transform, Bulk, or Realtime.
5. Add/adjust tests for packet priority and queue behavior.
6. Run network protocol/liveness/editor-sync tests, native collaborative loopback, frontend and SceneTools regressions, then a full RelWithDebInfo build.
7. Review diff, run `git diff --check`, and commit with Conventional Commits.
