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


class SlowEditor(FakeEditor):
    """An editor whose update consumes wall time, for the catch-up clamp."""

    def __init__(self, clock, cost):
        super().__init__()
        self.clock = clock
        self.cost = cost

    def update_runtime(self):
        super().update_runtime()
        self.clock.now += self.cost


def request(request_id, service, function, args):
    return {"request_id": request_id, "service": service, "function": function, "args": args}


def test_pending_work_is_completed_before_the_frame_sleeps():
    clock = FakeClock()
    engine = FakeEngine([[request(1, "project_files", "load", [{"path": "a"}])]], stop_after=1)
    editor = FakeEditor()
    snapshot = run_loop(engine, editor, clock=clock, sleep=clock.sleep,
                        dispatch=lambda s, f, a: {"success": True, "data": {"echoed": a}})
    assert engine.completed == [(1, {"success": True, "data": {"echoed": [{"path": "a"}]}})]
    assert clock.slept == []
    assert snapshot == {"runtime_state": "stopped"}
    assert editor.shutdown_calls == 1


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


def test_the_last_idle_sleep_is_truncated_to_the_update_deadline():
    clock = FakeClock()
    editor = FakeEditor()
    # 3/16 s polls inside a 1/2 s frame: the third poll can only sleep the 1/8 s
    # remainder, not another whole poll interval. Exact binary fractions keep the
    # expectation comparable to the fake clock's arithmetic.
    run_loop(FakeEngine([], stop_after=5), editor, clock=clock, sleep=clock.sleep,
             dispatch=lambda s, f, a: None, poll_interval=3 / 16, update_period=0.5)
    assert clock.slept == [0.1875, 0.1875, 0.125]
    assert editor.updates == 2


def test_a_slow_update_reanchors_the_next_deadline_instead_of_replaying_it():
    clock = FakeClock()
    editor = SlowEditor(clock, cost=0.05)
    run_loop(FakeEngine([], stop_after=4), editor, clock=clock, sleep=clock.sleep,
             dispatch=lambda s, f, a: None)
    # One 50ms update at t=0 leaves the 1/60s deadline 33ms in the past. The next
    # frame must wait one period from the finish (t=66.7ms), not replay the missed
    # deadline: three 4ms polls, no burst of catch-up updates.
    assert editor.updates == 1
    assert clock.slept == [pytest.approx(0.004)] * 3


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


def test_a_raising_completion_does_not_strand_the_rest_of_the_batch(caplog):
    clock = FakeClock()
    engine = FakeEngine([[request(1, "project_files", "load", []),
                          request(2, "project_files", "load", []),
                          request(3, "project_files", "load", [])]], stop_after=2)

    def complete(request_id, payload):
        if request_id == 1:
            raise RuntimeError("queue closed")
        engine.completed.append((request_id, payload))

    engine.complete_service_request = complete
    with caplog.at_level("ERROR", logger="runtime.host_loop"):
        run_loop(engine, FakeEditor(), clock=clock, sleep=clock.sleep,
                 dispatch=lambda s, f, a: {"success": True})
    ids = [request_id for request_id, _ in engine.completed]
    assert ids == [2, 3], "a completion failure must not strand the rest of the batch"
    assert "event=runtime.service.complete_failed request_id=1" in caplog.text
