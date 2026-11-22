"""The host loop owns the frame: pull work, tick the editor, sleep only when idle."""
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "embedded_python"))

from runtime.host_loop import run_loop  # noqa: E402


class FakeClock:
    def __init__(self):
        self.now = 0.0
        self.slept = []

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.slept.append(seconds)
        self.now += seconds


class FakeEngine:
    def __init__(self, batches, stop_after):
        self.batches = list(batches)
        self.stop_after = stop_after
        self.polls = 0
        self.completed = []

    def should_stop(self):
        return self.polls >= self.stop_after

    def poll_service_requests(self):
        self.polls += 1
        return self.batches.pop(0) if self.batches else []

    def complete_service_request(self, request_id, payload):
        self.completed.append((request_id, payload))


class FakeEditor:
    def __init__(self):
        self.updates = 0
        self.shutdown_calls = 0
        self.raise_on_update = False

    def update_runtime(self):
        self.updates += 1
        if self.raise_on_update:
            raise RuntimeError("boom")

    def shutdown_runtime(self):
        self.shutdown_calls += 1
        return {"runtime_state": "stopped"}


def request(request_id, service, function, args):
    return {"request_id": request_id, "service": service, "function": function, "args": args}


def test_pending_work_is_completed_before_the_frame_sleeps():
    clock = FakeClock()
    engine = FakeEngine([[request(1, "project_files", "load", [{"path": "a"}])]], stop_after=1)
    editor = FakeEditor()
    run_loop(engine, editor, clock=clock, sleep=clock.sleep,
             dispatch=lambda s, f, a: {"success": True, "data": {"echoed": a}})
    assert engine.completed == [(1, {"success": True, "data": {"echoed": [{"path": "a"}]}})]
    assert clock.slept == []


def test_an_idle_frame_sleeps_until_the_next_update_deadline():
    clock = FakeClock()
    run_loop(FakeEngine([], stop_after=2), FakeEditor(), clock=clock, sleep=clock.sleep,
             dispatch=lambda s, f, a: None)
    assert clock.slept == [pytest.approx(0.004)]


def test_the_update_period_is_paced_by_absolute_deadlines():
    clock = FakeClock()
    editor = FakeEditor()
    run_loop(FakeEngine([], stop_after=6), editor, clock=clock, sleep=clock.sleep,
             dispatch=lambda s, f, a: None)
    assert editor.updates == 1, "six idle polls at 4ms fit inside one 1/60s frame"


def test_shutdown_runs_even_when_the_update_raises_and_the_snapshot_is_returned():
    clock = FakeClock()
    editor = FakeEditor()
    editor.raise_on_update = True
    with pytest.raises(RuntimeError):
        run_loop(FakeEngine([], stop_after=1), editor, clock=clock, sleep=clock.sleep,
                 dispatch=lambda s, f, a: None)
    assert editor.shutdown_calls == 1


def test_a_raising_dispatch_still_completes_every_drained_request():
    clock = FakeClock()
    engine = FakeEngine([[request(1, "project_files", "load", []),
                          request(2, "project_files", "load", [])]], stop_after=2)

    def dispatch(service, function, args):
        raise RuntimeError("boom")

    run_loop(engine, FakeEditor(), clock=clock, sleep=clock.sleep, dispatch=dispatch)
    ids = [request_id for request_id, _ in engine.completed]
    assert ids == [1, 2], "every drained id must be settled even when dispatch raises"
    for _, payload in engine.completed:
        assert payload["success"] is False and payload["code"] == "SERVICE_FAILED"
