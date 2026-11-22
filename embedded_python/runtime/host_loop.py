"""The embedded Python host loop.

C++ used to call into Python once per frame. The script thread now enters the
runtime once (``PythonAPI::enter_runtime``), and this loop owns the frame:
drain queued service calls, tick the editor on a fixed period, and sleep only
while there is nothing to do.
"""

import logging
import time

logger = logging.getLogger(__name__)


def run_loop(engine, editor, *, dispatch, poll_interval=0.004,
             update_period=1.0 / 60.0, clock=time.perf_counter, sleep=time.sleep):
    """Pull work, tick the editor, and return the shutdown snapshot.

    ``dispatch`` is required: without it a drained request can never complete.
    The native module is injected as ``engine`` so this module stays importable
    without an interpreter.
    """
    next_update = clock()
    snapshot = None
    try:
        while not engine.should_stop():
            requests = engine.poll_service_requests()
            if requests:
                for request in requests:
                    request_id = request.get("request_id")
                    try:
                        payload = dispatch(
                            request.get("service"), request.get("function"),
                            request.get("args", []))
                    except Exception as error:
                        # One failing service must not strand the rest of the batch: the
                        # C++ caller waits in submit_and_wait() without a timeout, so a
                        # dropped completion hangs the editor thread forever.
                        payload = {
                            "success": False,
                            "code": "SERVICE_FAILED",
                            "error": str(error),
                        }
                    try:
                        engine.complete_service_request(request_id, payload)
                    except Exception:
                        # Completion can fail on its own (unconvertible payload, queue
                        # failure). That request's caller is already lost, but the other
                        # drained ids must still be settled, so log and keep going.
                        logger.exception(
                            "event=runtime.service.complete_failed request_id=%s", request_id)
                # Work arrived: give this frame to the editor instead of sleeping.
                next_update = clock()
                continue
            now = clock()
            if now >= next_update:
                editor.update_runtime()
                next_update += update_period
                # A frame that overran its period must not leave a backlog of catch-up
                # frames behind it: re-anchor the deadline instead of replaying them.
                finished = clock()
                if next_update <= finished:
                    next_update = finished + update_period
                continue
            sleep(min(poll_interval, next_update - now))
    finally:
        snapshot = editor.shutdown_runtime()
    return snapshot
