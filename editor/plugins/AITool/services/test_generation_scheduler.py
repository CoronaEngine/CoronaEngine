from __future__ import annotations

import os
import sys
import logging
import threading
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from plugins.AITool.services.generation_scheduler import (  # noqa: E402
    GenerationJob,
    GenerationScheduler,
)
from plugins.AITool.services.generation_composer_adapter import SceneComposerJobRunner  # noqa: E402
from plugins.AITool.services.generation_provider_adapter import (  # noqa: E402
    DeferredDownloadProvider,
    ProviderStageRunner,
)


def test_scheduler_runs_all_stages_in_order():
    calls = []

    def handler(stage):
        def _run(job: GenerationJob):
            calls.append(stage)
            return {stage: True}

        return _run

    scheduler = GenerationScheduler(
        stage_handlers={stage: handler(stage) for stage in ("prepare", "submit", "poll", "download", "postprocess", "import")},
    )
    try:
        submitted = scheduler.submit({"plan_id": "seed-1", "session_id": "room-a"})
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert calls == ["prepare", "submit", "poll", "download", "postprocess", "import"]
    print("[OK] GenerationScheduler runs stages in order")


def test_scheduler_logs_safe_job_lifecycle_ids_without_payload_leaks():
    records = []

    class ListHandler(logging.Handler):
        def emit(self, record):
            records.append(self.format(record))

    logger = logging.getLogger("plugins.AITool.services.generation_scheduler")
    handler = ListHandler()
    handler.setFormatter(logging.Formatter("%(message)s"))
    old_level = logger.level
    logger.setLevel(logging.INFO)
    logger.addHandler(handler)
    try:
        scheduler = GenerationScheduler(stage_order=("prepare",), auto_start=True)
        try:
            submitted = scheduler.submit({
                "room_id": "room-log",
                "session_id": "exec-log",
                "plan_id": "seed-log",
                "batch_id": "batch-log",
                "priority": 7,
                "prompt": "secret prompt should not leak",
                "provider": "secret-provider",
                "_runtime_context": {"token": "secret-token"},
            })
            final = scheduler.wait(submitted["job_id"], timeout=2.0)
        finally:
            scheduler.shutdown()
    finally:
        logger.removeHandler(handler)
        logger.setLevel(old_level)

    joined = "\n".join(records)
    assert final["status"] == "done"
    assert "event=submit" in joined
    assert "event=status_change" in joined
    assert "room_id=room-log" in joined
    assert "session_id=exec-log" in joined
    assert "plan_id=seed-log" in joined
    assert "job_id=" in joined
    assert "batch_id=batch-log" in joined
    assert "status=done" in joined
    assert "secret prompt" not in joined
    assert "secret-provider" not in joined
    assert "secret-token" not in joined
    print("[OK] GenerationScheduler logs safe lifecycle ids without payload leaks")


def test_scene_generation_without_stage_handler_fails_instead_of_fake_done():
    scheduler = GenerationScheduler(stage_order=("prepare",), auto_start=True)
    try:
        submitted = scheduler.submit({
            "room_id": "room-a",
            "session_id": "exec-missing-handler",
            "plan_id": "seed-missing-handler",
            "job_type": "scene_generation",
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "failed"
    assert "stage handler missing" in final["error"]
    assert final["result"].get("completed_stages") is None
    print("[OK] scene_generation without stage handler fails instead of fake done")


def test_scene_composer_paused_result_marks_job_paused_not_failed():
    class PausingComposer:
        def compose(self, *args, **kwargs):
            return {
                "paused": True,
                "paused_mode": "DISCUSSING",
                "paused_before_phase": "OBJECTS#1",
            }

    runner = SceneComposerJobRunner(lambda: PausingComposer())
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("compose",),
        auto_start=True,
    )
    try:
        submitted = scheduler.submit({
            "room_id": "room-paused-compose",
            "session_id": "exec-paused-compose",
            "plan_id": "seed-paused-compose",
            "job_type": "scene_generation",
            "prompt": "生成一个二战前线小型交战场地",
        })
        deadline = time.time() + 2.0
        current = scheduler.status(submitted["job_id"])
        while current["status"] in {"queued", "composing"} and time.time() < deadline:
            time.sleep(0.02)
            current = scheduler.status(submitted["job_id"])
    finally:
        scheduler.shutdown()

    assert current["status"] == "paused"
    assert current["error"] == ""
    assert current["result"]["compose_result"]["paused"] is True
    assert current["result"]["paused_before_phase"] == "OBJECTS#1"
    print("[OK] paused SceneComposer result keeps GenerationScheduler out of failed state")


def test_scheduler_cancel_before_submit_stops_download_and_import():
    calls = []

    def slow_prepare(job: GenerationJob):
        calls.append("prepare")
        time.sleep(0.05)

    scheduler = GenerationScheduler(stage_handlers={"prepare": slow_prepare})
    try:
        submitted = scheduler.submit({"plan_id": "seed-cancel", "session_id": "room-a"})
        cancelled = scheduler.cancel(submitted["job_id"])
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert cancelled["success"] is True
    assert final["status"] in {"cancelled", "abandoned"}
    assert "download" not in calls
    assert "import" not in calls
    print("[OK] GenerationScheduler cancel prevents later stages")


def test_scheduler_pause_session_blocks_until_resume():
    reached = []

    def prepare(job: GenerationJob):
        reached.append("prepare")

    def submit(job: GenerationJob):
        reached.append("submit")

    scheduler = GenerationScheduler(stage_handlers={"prepare": prepare, "submit": submit})
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({"plan_id": "seed-pause", "session_id": "room-a"})
        time.sleep(0.05)
        paused = scheduler.status(submitted["job_id"])
        assert paused["status"] == "paused"
        assert reached == []
        scheduler.resume_session("room-a")
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert reached[:2] == ["prepare", "submit"]
    print("[OK] GenerationScheduler pauses whole session at stage boundary")


def test_scheduler_cancel_paused_job_without_resume_releases_queue():
    reached = []
    scheduler = GenerationScheduler(
        stage_handlers={"prepare": lambda job: reached.append("prepare")},
        queue_limit=1,
    )
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({"plan_id": "seed-paused-cancel", "session_id": "room-a"})
        time.sleep(0.05)
        paused = scheduler.status(submitted["job_id"])
        cancelled = scheduler.cancel(submitted["job_id"])
        final = scheduler.wait(submitted["job_id"], timeout=1.0)
        snapshot = scheduler.snapshot()
    finally:
        scheduler.shutdown()

    assert paused["status"] == "paused"
    assert cancelled["success"] is True
    assert final["status"] in {"cancelled", "abandoned"}
    assert reached == []
    assert snapshot["queued_count"] == 0
    assert snapshot["queue_pressure"] == 0.0
    assert any(event["event_type"] == "cancel_requested" for event in snapshot["recent_events"])
    print("[OK] GenerationScheduler cancels paused jobs without waiting for resume")


def test_scheduler_cancel_session_cancels_queued_jobs_without_touching_other_rooms():
    started = threading.Event()
    release = threading.Event()

    def submit(job: GenerationJob):
        if job.session_id == "room-b":
            started.set()
            assert release.wait(timeout=1.0)

    scheduler = GenerationScheduler(stage_handlers={"submit": submit}, stage_order=("submit",))
    try:
        other = scheduler.submit({"plan_id": "seed-other", "session_id": "room-b"})
        assert started.wait(timeout=1.0)
        queued_a = scheduler.submit({"plan_id": "seed-a", "session_id": "room-a", "batch_id": "batch-a"})
        queued_b = scheduler.submit({"plan_id": "seed-b", "session_id": "room-a", "batch_id": "batch-b"})
        cancelled = scheduler.cancel_session("room-a")
        final_a = scheduler.wait(queued_a["job_id"], timeout=1.0)
        final_b = scheduler.wait(queued_b["job_id"], timeout=1.0)
        other_mid = scheduler.status(other["job_id"])
        release.set()
        other_final = scheduler.wait(other["job_id"], timeout=2.0)
        snapshot = scheduler.snapshot()
    finally:
        release.set()
        scheduler.shutdown()

    assert cancelled["success"] is True
    assert cancelled["cancelled_count"] == 2
    assert set(cancelled["job_ids"]) == {queued_a["job_id"], queued_b["job_id"]}
    assert final_a["status"] == "cancelled"
    assert final_b["status"] == "cancelled"
    assert other_mid["status"] == "submitting"
    assert other_final["status"] == "done"
    assert any(event["event_type"] == "cancel_session" for event in snapshot["recent_events"])
    print("[OK] GenerationScheduler cancel_session cancels queued room jobs only")


def test_scheduler_cancel_room_cancels_execution_session_jobs_without_touching_other_rooms():
    scheduler = GenerationScheduler(auto_start=True)
    try:
        scheduler.pause_session("room-a")
        room_a = scheduler.submit({
            "plan_id": "seed-room-a",
            "room_id": "room-a",
            "session_id": "exec-seed-room-a-1",
            "batch_id": "batch-a",
        })
        room_b = scheduler.submit({
            "plan_id": "seed-room-b",
            "room_id": "room-b",
            "session_id": "exec-seed-room-b-1",
            "batch_id": "batch-b",
        })
        cancelled = scheduler.cancel_session("room-a")
        final_a = scheduler.wait(room_a["job_id"], timeout=1.0)
        final_b = scheduler.wait(room_b["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert cancelled["success"] is True
    assert cancelled["cancelled_count"] == 1
    assert cancelled["job_ids"] == [room_a["job_id"]]
    assert final_a["status"] == "cancelled"
    assert final_b["status"] == "done"
    print("[OK] GenerationScheduler room cancel catches execution-session jobs only in that room")


def test_scheduler_cancel_session_cancels_paused_jobs_and_clears_pause():
    reached = []
    scheduler = GenerationScheduler(
        stage_handlers={"prepare": lambda job: reached.append(job.session_id)},
        queue_limit=2,
    )
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({"plan_id": "seed-paused-session", "session_id": "room-a"})
        time.sleep(0.05)
        paused = scheduler.status(submitted["job_id"])
        cancelled = scheduler.cancel_session("room-a")
        final = scheduler.wait(submitted["job_id"], timeout=1.0)
        snapshot = scheduler.snapshot()
    finally:
        scheduler.shutdown()

    assert paused["status"] == "paused"
    assert cancelled["success"] is True
    assert cancelled["cancelled_count"] == 1
    assert final["status"] == "cancelled"
    assert reached == []
    assert "room-a" not in snapshot["paused_sessions"]
    assert snapshot["queued_count"] == 0
    print("[OK] GenerationScheduler cancel_session cancels paused room jobs and clears pause")


def test_scheduler_cancel_session_abandons_running_job_when_requested():
    started = threading.Event()
    release = threading.Event()

    def submit(job: GenerationJob):
        started.set()
        assert release.wait(timeout=1.0)

    scheduler = GenerationScheduler(
        stage_handlers={"submit": submit},
        stage_order=("submit", "import"),
    )
    try:
        submitted = scheduler.submit({"plan_id": "seed-running-abandon", "session_id": "room-a"})
        assert started.wait(timeout=1.0)
        cancelled = scheduler.cancel_session("room-a", abandon_remote=True)
        running = scheduler.status(submitted["job_id"])
        release.set()
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        release.set()
        scheduler.shutdown()

    assert cancelled["success"] is True
    assert cancelled["job_ids"] == [submitted["job_id"]]
    assert running["cancel_requested"] is True
    assert running["abandon_requested"] is True
    assert final["status"] == "abandoned"
    assert final["error"] == "cancel requested"
    print("[OK] GenerationScheduler cancel_session preserves abandon semantics for running jobs")


def test_scheduler_import_stage_is_serialized():
    active = 0
    max_active = 0
    lock = threading.Lock()

    def import_handler(job: GenerationJob):
        nonlocal active, max_active
        with lock:
            active += 1
            max_active = max(max_active, active)
        time.sleep(0.03)
        with lock:
            active -= 1

    scheduler = GenerationScheduler(
        stage_handlers={"import": import_handler},
        stage_order=("import",),
        concurrency={"import": 1},
    )
    try:
        job_a = scheduler.submit({"plan_id": "seed-a", "session_id": "room-a"})
        job_b = scheduler.submit({"plan_id": "seed-b", "session_id": "room-b"})
        final_a = scheduler.wait(job_a["job_id"], timeout=2.0)
        final_b = scheduler.wait(job_b["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final_a["status"] == "done"
    assert final_b["status"] == "done"
    assert max_active == 1
    print("[OK] GenerationScheduler serializes import stage")


def test_scheduler_queue_limit_applies_backpressure():
    scheduler = GenerationScheduler(queue_limit=1, auto_start=True)
    try:
        scheduler.pause_session("room-a")
        first = scheduler.submit({"plan_id": "seed-a", "session_id": "room-a"})
        second = scheduler.submit({"plan_id": "seed-b", "session_id": "room-a"})
    finally:
        scheduler.shutdown()

    assert first["job_id"]
    assert second["status"] == "waiting_user"
    print("[OK] GenerationScheduler exposes queue backpressure status")


def test_scheduler_rejects_submit_after_shutdown_without_restarting_worker():
    scheduler = GenerationScheduler(auto_start=True)
    scheduler.shutdown()

    rejected = scheduler.submit({
        "plan_id": "seed-late",
        "session_id": "room-a",
        "prompt": "private late prompt",
    })
    snapshot = scheduler.snapshot()

    assert rejected["job_id"] == ""
    assert rejected["status"] == "rejected"
    assert rejected["success"] is False
    assert "shut down" in rejected["error"]
    assert snapshot["thread_alive"] is False
    assert snapshot["shutdown_requested"] is True
    assert snapshot["diagnosis"]["state"] == "stopped"
    assert "scheduler_stopped" in snapshot["diagnosis"]["reasons"]
    assert "start_a_new_generation_session" in snapshot["diagnosis"]["recommended_actions"]
    assert snapshot["total_jobs"] == 0
    assert snapshot["queued_count"] == 0
    assert any(
        event["event_type"] == "submit_rejected_after_shutdown"
        and event["plan_id"] == "seed-late"
        for event in snapshot["recent_events"]
    )
    assert "private late prompt" not in str(snapshot)
    print("[OK] GenerationScheduler rejects submit after shutdown without restarting worker")


def test_scheduler_snapshot_reports_paused_backpressure_without_payload_leak():
    marker = object()
    scheduler = GenerationScheduler(queue_limit=2, auto_start=True)
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({
            "plan_id": "seed-observe",
            "session_id": "room-a",
            "batch_id": "batch-1",
            "prompt": "private prompt should not appear in snapshot",
            "interaction_coordinator": marker,
        })
        time.sleep(0.05)
        snapshot = scheduler.snapshot()
    finally:
        scheduler.shutdown()

    snapshot_text = str(snapshot)
    assert snapshot["thread_alive"] is True
    assert snapshot["queue_limit"] == 2
    assert snapshot["queued_count"] == 1
    assert snapshot["queue_pressure"] == 0.5
    assert snapshot["paused_sessions"] == ["room-a"]
    assert snapshot["status_counts"]["paused"] == 1
    assert snapshot["active_jobs"][0]["job_id"] == submitted["job_id"]
    assert snapshot["active_jobs"][0]["batch_id"] == "batch-1"
    assert "private prompt" not in snapshot_text
    assert "interaction_coordinator" not in snapshot_text
    assert "object at" not in snapshot_text
    print("[OK] GenerationScheduler snapshot reports paused backpressure without payload leak")


def test_scheduler_snapshot_reports_active_stage_and_priority_queue():
    started = threading.Event()
    release = threading.Event()

    def submit(job: GenerationJob):
        started.set()
        assert release.wait(timeout=1.0)

    scheduler = GenerationScheduler(stage_handlers={"submit": submit}, stage_order=("submit",))
    try:
        active = scheduler.submit({"plan_id": "seed-active", "session_id": "room-a", "priority": 0})
        assert started.wait(timeout=1.0)
        low = scheduler.submit({"plan_id": "seed-low", "session_id": "room-b", "priority": 1})
        high = scheduler.submit({"plan_id": "seed-high", "session_id": "room-b", "priority": 9})
        snapshot = scheduler.snapshot()
        release.set()
        final_active = scheduler.wait(active["job_id"], timeout=2.0)
        final_high = scheduler.wait(high["job_id"], timeout=2.0)
        final_low = scheduler.wait(low["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final_active["status"] == "done"
    assert final_high["status"] == "done"
    assert final_low["status"] == "done"
    assert snapshot["stage_counts"]["submit"] == 1
    assert snapshot["active_jobs"][0]["job_id"] == active["job_id"]
    assert [item["job_id"] for item in snapshot["queued_jobs"]] == [high["job_id"], low["job_id"]]
    print("[OK] GenerationScheduler snapshot reports active stage and priority queue")


def test_scheduler_snapshot_includes_safe_recent_events():
    scheduler = GenerationScheduler(queue_limit=1, auto_start=True)
    try:
        scheduler.pause_session("room-a")
        first = scheduler.submit({
            "plan_id": "seed-events",
            "session_id": "room-a",
            "batch_id": "batch-events",
            "prompt": "private event prompt",
        })
        rejected = scheduler.submit({
            "plan_id": "seed-rejected",
            "session_id": "room-a",
            "prompt": "rejected private prompt",
        })
        time.sleep(0.05)
        scheduler.resume_session("room-a")
        final = scheduler.wait(first["job_id"], timeout=2.0)
        snapshot = scheduler.snapshot()
    finally:
        scheduler.shutdown()

    events = snapshot["recent_events"]
    event_types = [event["event_type"] for event in events]
    events_text = str(events)
    assert rejected["status"] == "waiting_user"
    assert final["status"] == "done"
    assert "pause_session" in event_types
    assert "submit" in event_types
    assert "queue_full" in event_types
    assert "resume_session" in event_types
    assert "status_change" in event_types
    assert "private event prompt" not in events_text
    assert "rejected private prompt" not in events_text
    assert any(event.get("batch_id") == "batch-events" for event in events)
    print("[OK] GenerationScheduler snapshot includes safe recent events")


def test_scheduler_snapshot_includes_payload_safe_resource_diagnosis():
    marker = object()
    scheduler = GenerationScheduler(queue_limit=1, auto_start=True)
    try:
        scheduler.pause_session("room-a")
        first = scheduler.submit({
            "plan_id": "seed-diagnosis",
            "session_id": "room-a",
            "prompt": "private diagnosis prompt",
            "interaction_coordinator": marker,
        })
        rejected = scheduler.submit({
            "plan_id": "seed-overflow",
            "session_id": "room-a",
            "prompt": "overflow private prompt",
        })
        time.sleep(0.05)
        snapshot = scheduler.snapshot()
    finally:
        scheduler.shutdown()

    diagnosis = snapshot["diagnosis"]
    diagnosis_text = str(diagnosis)
    snapshot_text = str(snapshot)
    assert first["job_id"]
    assert rejected["status"] == "waiting_user"
    assert diagnosis["state"] == "paused"
    assert "paused_sessions" in diagnosis["reasons"]
    assert "recent_queue_full" in diagnosis["reasons"]
    assert "queue_at_capacity" in diagnosis["reasons"]
    assert "resume_or_cancel_paused_sessions" in diagnosis["recommended_actions"]
    assert "wait_or_cancel_low_priority_jobs" in diagnosis["recommended_actions"]
    assert diagnosis["latest_queue_full_at"] > 0
    assert "private diagnosis prompt" not in snapshot_text
    assert "overflow private prompt" not in snapshot_text
    assert "interaction_coordinator" not in snapshot_text
    assert "object at" not in diagnosis_text
    print("[OK] GenerationScheduler snapshot includes payload-safe resource diagnosis")


def test_scheduler_session_snapshot_reports_room_level_resource_state_without_payload_leak():
    started = threading.Event()
    release = threading.Event()
    marker = object()

    def submit(job: GenerationJob):
        if job.session_id == "room-a":
            started.set()
            assert release.wait(timeout=1.0)

    scheduler = GenerationScheduler(
        stage_handlers={"submit": submit},
        stage_order=("submit",),
        queue_limit=3,
    )
    try:
        active = scheduler.submit({
            "plan_id": "seed-a",
            "session_id": "room-a",
            "batch_id": "batch-1",
            "prompt": "private active prompt",
            "interaction_coordinator": marker,
        })
        assert started.wait(timeout=1.0)
        queued = scheduler.submit({
            "plan_id": "seed-a",
            "session_id": "room-a",
            "batch_id": "batch-2",
            "prompt": "private queued prompt",
            "priority": 5,
        })
        other = scheduler.submit({
            "plan_id": "seed-b",
            "session_id": "room-b",
            "batch_id": "batch-other",
            "prompt": "other room prompt",
        })
        scheduler.pause_session("room-a")
        session = scheduler.session_snapshot("room-a")
        scheduler.resume_session("room-a")
        release.set()
        final_active = scheduler.wait(active["job_id"], timeout=2.0)
        final_queued = scheduler.wait(queued["job_id"], timeout=2.0)
        final_other = scheduler.wait(other["job_id"], timeout=2.0)
    finally:
        release.set()
        scheduler.shutdown()

    session_text = str(session)
    assert final_active["status"] == "done"
    assert final_queued["status"] == "done"
    assert final_other["status"] == "done"
    assert session["session_id"] == "room-a"
    assert session["state"] == "paused"
    assert session["paused"] is True
    assert session["queue_limit"] == 3
    assert session["queued_count"] == 1
    assert session["active_count"] == 1
    assert session["total_jobs"] == 2
    assert session["diagnosis"]["state"] == "paused"
    assert "paused_sessions" in session["diagnosis"]["reasons"]
    assert [item["batch_id"] for item in session["queued_jobs"]] == ["batch-2"]
    assert session["active_jobs"][0]["batch_id"] == "batch-1"
    assert "private active prompt" not in session_text
    assert "private queued prompt" not in session_text
    assert "other room prompt" not in session_text
    assert "interaction_coordinator" not in session_text
    assert "object at" not in session_text
    print("[OK] GenerationScheduler session_snapshot reports room-level resource state without payload leak")


def test_scheduler_room_snapshot_includes_execution_session_jobs_without_payload_leak():
    marker = object()
    scheduler = GenerationScheduler(queue_limit=3, auto_start=True)
    try:
        scheduler.pause_session("room-a")
        active = scheduler.submit({
            "plan_id": "seed-a",
            "room_id": "room-a",
            "session_id": "exec-seed-a-1",
            "batch_id": "batch-1",
            "prompt": "private execution prompt",
            "interaction_coordinator": marker,
        })
        other = scheduler.submit({
            "plan_id": "seed-b",
            "room_id": "room-b",
            "session_id": "exec-seed-b-1",
            "batch_id": "batch-other",
            "prompt": "other private prompt",
        })
        time.sleep(0.05)
        session = scheduler.session_snapshot("room-a")
        scheduler.resume_session("room-a")
        final_active = scheduler.wait(active["job_id"], timeout=2.0)
        final_other = scheduler.wait(other["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    session_text = str(session)
    assert final_active["status"] == "done"
    assert final_other["status"] == "done"
    assert session["session_id"] == "room-a"
    assert session["state"] == "paused"
    assert session["paused"] is True
    assert session["queued_count"] == 1
    assert session["total_jobs"] == 1
    assert session["active_jobs"][0]["room_id"] == "room-a"
    assert session["active_jobs"][0]["session_id"] == "exec-seed-a-1"
    assert "private execution prompt" not in session_text
    assert "other private prompt" not in session_text
    assert "interaction_coordinator" not in session_text
    print("[OK] GenerationScheduler room snapshot includes execution-session jobs without payload leak")


def test_scheduler_public_snapshots_hide_execution_ids_and_payload_fields():
    marker = object()
    scheduler = GenerationScheduler(queue_limit=2, auto_start=True)
    try:
        scheduler.pause_session("room-a")
        submitted = scheduler.submit({
            "job_id": "gen-private-job",
            "plan_id": "seed-private-plan",
            "room_id": "room-a",
            "session_id": "exec-private-session",
            "batch_id": "batch-private",
            "prompt": "private public snapshot prompt",
            "provider": "private-provider",
            "token": "private-token",
            "vlm_raw": "private-vlm-raw",
            "interaction_coordinator": marker,
        })
        time.sleep(0.05)
        internal = scheduler.session_snapshot("room-a")
        public_room = scheduler.public_session_snapshot("room-a")
        public_global = scheduler.public_snapshot()
    finally:
        scheduler.shutdown()

    assert internal["active_jobs"][0]["job_id"] == submitted["job_id"]
    assert internal["active_jobs"][0]["session_id"] == "exec-private-session"
    exposed = str(public_room) + str(public_global)
    assert public_room["state"] == "paused"
    assert public_room["paused_session_count"] == 1
    assert public_room["active_jobs"][0]["room_id"] == "room-a"
    assert public_room["active_jobs"][0]["status"] == "paused"
    assert public_room["recent_events"]
    assert "private public snapshot prompt" not in exposed
    assert "private-provider" not in exposed
    assert "private-token" not in exposed
    assert "private-vlm-raw" not in exposed
    assert "interaction_coordinator" not in exposed
    assert "object at" not in exposed
    assert "gen-private-job" not in exposed
    assert "exec-private-session" not in exposed
    assert "seed-private-plan" not in exposed
    assert "batch-private" not in exposed
    print("[OK] GenerationScheduler public snapshots hide execution ids and payload fields")


def test_scheduler_runs_higher_priority_queued_job_first():
    calls = []
    first_started = threading.Event()
    release_first = threading.Event()

    def submit(job: GenerationJob):
        calls.append(job.payload["name"])
        if job.payload["name"] == "first":
            first_started.set()
            assert release_first.wait(timeout=1.0)

    scheduler = GenerationScheduler(stage_handlers={"submit": submit}, stage_order=("submit",))
    try:
        first = scheduler.submit({"plan_id": "seed-first", "name": "first", "priority": 0})
        assert first_started.wait(timeout=1.0)
        low = scheduler.submit({"plan_id": "seed-low", "name": "low", "priority": 0})
        high = scheduler.submit({"plan_id": "seed-high", "name": "high", "priority": 10})
        release_first.set()
        final_first = scheduler.wait(first["job_id"], timeout=2.0)
        final_high = scheduler.wait(high["job_id"], timeout=2.0)
        final_low = scheduler.wait(low["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final_first["status"] == "done"
    assert final_high["status"] == "done"
    assert final_low["status"] == "done"
    assert calls == ["first", "high", "low"]
    print("[OK] GenerationScheduler prioritizes queued future batches")


def test_scheduler_updates_queued_job_before_execution():
    calls = []
    first_started = threading.Event()
    release_first = threading.Event()

    def submit(job: GenerationJob):
        calls.append((job.payload["name"], job.payload.get("prompt"), job.priority))
        if job.payload["name"] == "first":
            first_started.set()
            assert release_first.wait(timeout=1.0)

    scheduler = GenerationScheduler(stage_handlers={"submit": submit}, stage_order=("submit",))
    try:
        first = scheduler.submit({"plan_id": "seed-first", "name": "first", "priority": 0})
        assert first_started.wait(timeout=1.0)
        queued = scheduler.submit({
            "plan_id": "seed-edit",
            "name": "edited",
            "prompt": "old layout",
            "priority": 0,
        })
        updated = scheduler.update_job(
            queued["job_id"],
            priority=8,
            payload_updates={"prompt": "new user intervention layout"},
        )
        release_first.set()
        final_first = scheduler.wait(first["job_id"], timeout=2.0)
        final_queued = scheduler.wait(queued["job_id"], timeout=2.0)
        update_after_start = scheduler.update_job(queued["job_id"], payload_updates={"prompt": "too late"})
    finally:
        scheduler.shutdown()

    assert updated["success"] is True
    assert updated["priority"] == 8
    assert updated["payload"]["prompt"] == "new user intervention layout"
    assert final_first["status"] == "done"
    assert final_queued["status"] == "done"
    assert calls == [
        ("first", None, 0),
        ("edited", "new user intervention layout", 8),
    ]
    assert update_after_start["success"] is False
    assert update_after_start["status"] == "done"
    print("[OK] GenerationScheduler updates queued future batch payload and priority")


def test_scheduler_keeps_runtime_context_out_of_public_payload():
    marker = object()
    seen = []

    def compose(job: GenerationJob):
        seen.append(job.runtime_context["interaction_coordinator"])
        return {"ok": True}

    scheduler = GenerationScheduler(stage_handlers={"compose": compose}, stage_order=("compose",))
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-runtime",
            "session_id": "room-a",
            "interaction_coordinator": marker,
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert seen == [marker]
    assert "interaction_coordinator" not in final["payload"]
    assert final["current_stage"] == ""
    assert final["result"]["ok"] is True
    print("[OK] GenerationScheduler keeps runtime objects out of public job payload")


def test_scheduler_prunes_terminal_job_history_without_payload_leak():
    scheduler = GenerationScheduler(
        stage_order=("prepare",),
        max_retained_terminal_jobs=2,
    )
    try:
        submitted = []
        finals = []
        for index in range(4):
            item = scheduler.submit({
                "plan_id": f"seed-retain-{index}",
                "session_id": "room-a",
                "prompt": f"private retained prompt {index}",
            })
            submitted.append(item)
            finals.append(scheduler.wait(item["job_id"], timeout=2.0))
        snapshot = scheduler.snapshot()
    finally:
        scheduler.shutdown()

    assert [item["status"] for item in finals] == ["done", "done", "done", "done"]
    assert snapshot["total_jobs"] == 2
    assert snapshot["retained_terminal_job_limit"] == 2
    assert snapshot["pruned_terminal_jobs"] == 2
    assert scheduler.status(submitted[0]["job_id"])["status"] == "not_found"
    assert scheduler.status(submitted[1]["job_id"])["status"] == "not_found"
    assert scheduler.status(submitted[2]["job_id"])["status"] == "done"
    assert scheduler.status(submitted[3]["job_id"])["status"] == "done"
    assert "terminal_jobs_pruned" in [event["event_type"] for event in snapshot["recent_events"]]
    assert "private retained prompt" not in str(snapshot)
    print("[OK] GenerationScheduler prunes terminal job history without payload leak")


def test_scene_composer_job_runner_passes_seed_plan_context_to_compose():
    calls = []

    class FakeComposer:
        def compose(self, text, **kwargs):
            calls.append((text, kwargs))
            coordinator = kwargs["interaction_coordinator"]
            coordinator.bind_scene_session_progress(
                "fake-session",
                room_id=kwargs["room_id"],
                plan_id=kwargs["plan_id"],
                session_id=kwargs["session_id"],
            )
            return {"imported": ["asset-a"], "progressive": True}

    class FakeCoordinator:
        def __init__(self):
            self.bound = []

        def bind_scene_session_progress(self, session, *, room_id, plan_id, session_id=""):
            self.bound.append((session, room_id, plan_id, session_id))

    coordinator = FakeCoordinator()
    runner = SceneComposerJobRunner(lambda: FakeComposer())
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("compose",),
    )
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-compose",
            "session_id": "sess-1",
            "room_id": "room-a",
            "seed_plan": {"intent_summary": "室外暗黑集市"},
            "_runtime_context": {"interaction_coordinator": coordinator},
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert calls[0][0] == "室外暗黑集市"
    assert calls[0][1]["room_id"] == "room-a"
    assert calls[0][1]["plan_id"] == "seed-compose"
    assert calls[0][1]["session_id"] == "sess-1"
    assert calls[0][1]["interaction_coordinator"] is coordinator
    assert coordinator.bound == [("fake-session", "room-a", "seed-compose", "sess-1")]
    assert final["result"]["compose_result"]["imported"] == ["asset-a"]
    print("[OK] SceneComposerJobRunner passes SeedPlan context into compose")


def test_scene_composer_job_runner_prefers_seed_plan_design_brief_over_short_trigger():
    calls = []

    class FakeComposer:
        def compose(self, text, **kwargs):
            calls.append((text, kwargs))
            return {"imported": ["asset-guild-hall"], "progressive": True}

    runner = SceneComposerJobRunner(lambda: FakeComposer())
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("compose",),
    )
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-compose-brief",
            "session_id": "sess-brief",
            "room_id": "room-a",
            "intent_text": "按照这个方案生成",
            "seed_plan": {
                "intent_summary": "按照这个方案生成",
                "design_brief": "商会主交易厅，室内大厅，以中心议价区为核心，后方设权威徽记背景。",
                "design_items": ["议价桌", "长椅", "徽记墙", "展示台"],
            },
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert "商会主交易厅" in calls[0][0]
    assert "议价桌、长椅、徽记墙、展示台" in calls[0][0]
    assert "按照这个方案生成" not in calls[0][0]
    print("[OK] SceneComposerJobRunner prefers SeedPlan design brief over short trigger")


def test_scene_composer_job_runner_passes_target_actor_from_intervention():
    calls = []

    class FakeComposer:
        def compose(self, text, **kwargs):
            calls.append((text, kwargs))
            return {"imported": ["actor-statue"]}

    runner = SceneComposerJobRunner(lambda: FakeComposer())
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("compose",),
    )
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-compose-actor",
            "session_id": "sess-actor",
            "room_id": "room-a",
            "seed_plan": {"intent_summary": "室外暗黑集市"},
            "latest_intervention": {
                "actor_id": "actor-statue",
                "content": "雕像缩小一些",
            },
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert calls[0][1]["actor_id"] == "actor-statue"
    assert final["result"]["compose_result"]["imported"] == ["actor-statue"]
    print("[OK] SceneComposerJobRunner passes target actor from intervention into compose")


def test_scene_composer_job_runner_limits_append_job_and_keeps_style_contract():
    calls = []

    class FakeComposer:
        def __init__(self):
            self.max_items = 8

        def compose(self, text, **kwargs):
            calls.append({
                "text": text,
                "max_items": self.max_items,
                "kwargs": kwargs,
            })
            return {"imported": ["actor-angel-statue"]}

    runner = SceneComposerJobRunner(lambda: FakeComposer())
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("compose",),
    )
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-compose-append",
            "session_id": "append-seed-compose-append",
            "room_id": "room-a",
            "action_type": "post_generation_add",
            "append_mode": True,
            "max_items": 2,
            "intent_text": "添加生成一个天使雕像",
            "scene_design_contract": {
                "asset_style_prompt": "warm mysterious fantasy night market, not horror",
            },
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert calls[0]["max_items"] == 2
    assert "只追加本次新增对象" in calls[0]["text"]
    assert "添加生成一个天使雕像" in calls[0]["text"]
    assert "warm mysterious fantasy night market" in calls[0]["text"]
    assert final["result"]["compose_result"]["imported"] == ["actor-angel-statue"]
    print("[OK] SceneComposerJobRunner constrains post-generation append jobs")


def test_scene_composer_job_runner_pauses_progressive_compose_without_failure():
    class FakeComposer:
        def compose(self, text, **kwargs):
            return {
                "paused": True,
                "paused_mode": "DISCUSSING",
                "paused_before_phase": "INTERIOR",
                "imported": [],
            }

    runner = SceneComposerJobRunner(lambda: FakeComposer())
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("compose",),
    )
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-compose-paused",
            "session_id": "sess-paused",
            "room_id": "room-a",
            "seed_plan": {"intent_summary": "室外夜市"},
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "paused"
    assert final.get("error", "") == ""
    assert final["result"]["compose_result"]["paused"] is True
    assert final["result"]["paused_mode"] == "DISCUSSING"
    assert final["result"]["paused_before_phase"] == "INTERIOR"
    print("[OK] SceneComposerJobRunner pauses progressive compose without failing")


def test_provider_stage_runner_maps_provider_lifecycle_to_scheduler():
    calls = []

    class FakeProvider:
        def prepare(self, context):
            calls.append(("prepare", context["payload"]["prompt"]))
            return {"prepared_prompt": context["payload"]["prompt"].upper()}

        def submit(self, context):
            calls.append(("submit", context["result"]["prepared_prompt"]))
            return {"remote_task_id": "remote-1"}

        def poll(self, context):
            calls.append(("poll", context["result"]["remote_task_id"]))
            return {"remote_status": "succeeded"}

        def download(self, context):
            calls.append(("download", context["result"]["remote_status"]))
            return {"downloads": [{"name": "mesh", "url": "https://example.test/a.glb"}]}

        def postprocess(self, context):
            calls.append(("postprocess", len(context["result"]["downloads"])))
            return {"local_files": ["assets/model/a.glb"]}

        def import_result(self, context):
            calls.append(("import", context["result"]["local_files"][0]))
            return {"imported": ["actor-a"]}

    runner = ProviderStageRunner(lambda: FakeProvider())
    scheduler = GenerationScheduler(stage_handlers=runner.stage_handlers())
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-provider",
            "session_id": "room-a",
            "prompt": "dark market statue",
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert calls == [
        ("prepare", "dark market statue"),
        ("submit", "DARK MARKET STATUE"),
        ("poll", "remote-1"),
        ("download", "succeeded"),
        ("postprocess", 1),
        ("import", "assets/model/a.glb"),
    ]
    assert final["result"]["provider_completed_stages"] == [
        "prepare",
        "submit",
        "poll",
        "download",
        "postprocess",
        "import",
    ]
    assert final["result"]["imported"] == ["actor-a"]
    print("[OK] ProviderStageRunner maps provider lifecycle to scheduler stages")


def test_provider_stage_runner_uses_runtime_provider_without_payload_leak():
    class RuntimeProvider:
        def submit(self, context):
            assert context["runtime_context"]["generation_provider"] is self
            return {"remote_task_id": "runtime-task"}

    provider = RuntimeProvider()
    runner = ProviderStageRunner(lambda: (_ for _ in ()).throw(RuntimeError("factory should not run")))
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("submit",),
    )
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-runtime-provider",
            "_runtime_context": {"generation_provider": provider},
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert final["result"]["remote_task_id"] == "runtime-task"
    assert "generation_provider" not in final["payload"]
    print("[OK] ProviderStageRunner keeps runtime provider out of public payload")


def test_deferred_download_provider_runs_under_scheduler_download_stage():
    calls = []
    active = 0
    max_active = 0
    lock = threading.Lock()

    def legacy_download(**kwargs):
        nonlocal active, max_active
        with lock:
            active += 1
            max_active = max(max_active, active)
        try:
            time.sleep(0.03)
            calls.append(kwargs["object_dir_name"])
            return {"registered": [kwargs["object_dir_name"]]}
        finally:
            with lock:
                active -= 1

    runner = ProviderStageRunner(lambda: DeferredDownloadProvider(legacy_download))
    scheduler = GenerationScheduler(
        stage_handlers=runner.stage_handlers(),
        stage_order=("download",),
        concurrency={"download": 1},
    )
    try:
        job_a = scheduler.submit({
            "plan_id": "seed-download-a",
            "download_kwargs": {"object_dir_name": "model-a"},
        })
        job_b = scheduler.submit({
            "plan_id": "seed-download-b",
            "download_kwargs": {"object_dir_name": "model-b"},
        })
        final_a = scheduler.wait(job_a["job_id"], timeout=2.0)
        final_b = scheduler.wait(job_b["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final_a["status"] == "done"
    assert final_b["status"] == "done"
    assert calls == ["model-a", "model-b"]
    assert max_active == 1
    assert final_a["result"]["deferred_download_done"] is True
    assert final_a["result"]["download_result"]["registered"] == ["model-a"]
    print("[OK] DeferredDownloadProvider runs legacy downloads under scheduler limit")


def test_scheduler_accepts_per_job_stage_handlers():
    calls = []

    def download(job: GenerationJob):
        calls.append(job.payload["download_kwargs"]["object_dir_name"])
        return {"downloaded": True}

    scheduler = GenerationScheduler(stage_order=("prepare",))
    try:
        submitted = scheduler.submit({
            "plan_id": "seed-per-job",
            "download_kwargs": {"object_dir_name": "rodin-mesh"},
            "_runtime_context": {
                "stage_order": ("download",),
                "stage_handlers": {"download": download},
            },
        })
        final = scheduler.wait(submitted["job_id"], timeout=2.0)
    finally:
        scheduler.shutdown()

    assert final["status"] == "done"
    assert calls == ["rodin-mesh"]
    assert final["result"]["downloaded"] is True
    assert final["result"].get("completed_stages") is None
    print("[OK] GenerationScheduler accepts per-job stage handlers")


if __name__ == "__main__":
    test_scheduler_runs_all_stages_in_order()
    test_scheduler_logs_safe_job_lifecycle_ids_without_payload_leaks()
    test_scene_generation_without_stage_handler_fails_instead_of_fake_done()
    test_scheduler_cancel_before_submit_stops_download_and_import()
    test_scheduler_pause_session_blocks_until_resume()
    test_scheduler_cancel_paused_job_without_resume_releases_queue()
    test_scheduler_cancel_session_cancels_queued_jobs_without_touching_other_rooms()
    test_scheduler_cancel_room_cancels_execution_session_jobs_without_touching_other_rooms()
    test_scheduler_cancel_session_cancels_paused_jobs_and_clears_pause()
    test_scheduler_cancel_session_abandons_running_job_when_requested()
    test_scheduler_import_stage_is_serialized()
    test_scheduler_queue_limit_applies_backpressure()
    test_scheduler_rejects_submit_after_shutdown_without_restarting_worker()
    test_scheduler_snapshot_reports_paused_backpressure_without_payload_leak()
    test_scheduler_snapshot_reports_active_stage_and_priority_queue()
    test_scheduler_snapshot_includes_safe_recent_events()
    test_scheduler_snapshot_includes_payload_safe_resource_diagnosis()
    test_scheduler_session_snapshot_reports_room_level_resource_state_without_payload_leak()
    test_scheduler_room_snapshot_includes_execution_session_jobs_without_payload_leak()
    test_scheduler_public_snapshots_hide_execution_ids_and_payload_fields()
    test_scheduler_runs_higher_priority_queued_job_first()
    test_scheduler_updates_queued_job_before_execution()
    test_scheduler_keeps_runtime_context_out_of_public_payload()
    test_scheduler_prunes_terminal_job_history_without_payload_leak()
    test_scene_composer_job_runner_passes_seed_plan_context_to_compose()
    test_scene_composer_job_runner_prefers_seed_plan_design_brief_over_short_trigger()
    test_scene_composer_job_runner_passes_target_actor_from_intervention()
    test_scene_composer_job_runner_limits_append_job_and_keeps_style_contract()
    test_scene_composer_job_runner_pauses_progressive_compose_without_failure()
    test_scene_composer_paused_result_marks_job_paused_not_failed()
    test_provider_stage_runner_maps_provider_lifecycle_to_scheduler()
    test_provider_stage_runner_uses_runtime_provider_without_payload_leak()
    test_deferred_download_provider_runs_under_scheduler_download_stage()
    test_scheduler_accepts_per_job_stage_handlers()
