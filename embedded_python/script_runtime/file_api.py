"""Input, live Actor transforms and progress markers for a file executing in this host."""
from __future__ import annotations

import queue
import threading
import time
from contextlib import contextmanager

_tls = threading.local()


class FileRun:
    def __init__(self, run_id, session, target_type, scene_name, actor_name):
        self.id = run_id
        self.session = session
        self.target_type = target_type
        self.scene_name = scene_name
        self.actor_name = actor_name
        self.stopped = threading.Event()
        self.events = queue.Queue()
        self.key_handler = None
        self.keys = set()
        self.last_press = {}
        self.marker = ("", "")
        self.waiting = ("", "")
        self.on_input = None
        self.on_stop = None
        self.on_close = None
        self.on_quarantine = None
        self.child_threads = lambda: ()
        self.keep_alive = lambda: self.key_handler is not None
        self.result = lambda: None
        self.restore_on_exit = False

    def stop(self):
        self.stopped.set()
        self.events.put(None)
        if self.on_stop:
            self.on_stop()

    def send_input(self, kind, *args):
        if not self.stopped.is_set():
            self.events.put((kind, args))

    def dispatch(self, event):
        kind, args = event
        if kind in ("key_down", "key_up"):
            code, mods, display = args
            names = {str(code).casefold(), str(display or code).casefold()}
            if kind == "key_down":
                self.keys.update(names)
                self.last_press.update(dict.fromkeys(names, time.monotonic()))
                if self.key_handler:
                    self.key_handler(code, mods)
            else:
                self.keys.difference_update(names)
        if self.on_input:
            self.on_input(kind, *args)

    def release(self):
        self.stopped.set()
        self.key_handler = None
        self.keys.clear()
        self.last_press.clear()
        if self.on_close:
            self.on_close()


@contextmanager
def bind(run):
    _tls.run = run
    try:
        yield
    finally:
        run.release()
        del _tls.run


def current_run():
    return getattr(_tls, "run", None)


def _run():
    run = current_run()
    if run is None:
        raise RuntimeError("No file is running on this thread")
    if run.stopped.is_set():
        raise SystemExit(0)
    return run


def check_stop():
    _run()


def mark(marker_id, name=""):
    _run().marker = (marker_id, name)
    _run().waiting = ("", "")


def mark_waiting(marker_id="", name=""):
    _run().waiting = (marker_id, name)


def register_key_handler(handler):
    if not callable(handler):
        raise TypeError("Expected a key handler")
    _run().key_handler = handler


def keyboard(key):
    run = _run()
    name = str(key).casefold()
    return name in run.keys or time.monotonic() - run.last_press.get(name, float("-inf")) < 0.18


def wait(seconds):
    """Wait cooperatively; input handlers execute on the file's own thread."""
    run = _run()
    deadline = time.monotonic() + max(0, seconds)
    while True:
        check_stop()
        try:
            event = run.events.get(timeout=max(0, deadline - time.monotonic()))
        except queue.Empty:
            return
        check_stop()
        if event:
            run.dispatch(event)
        if time.monotonic() >= deadline:
            return


def translate_actor(name, dx, dy, dz):
    from api.editor_api import get_script_runtime_editor_api

    run = _run()
    scene = get_script_runtime_editor_api().scene
    actor = scene.read_actor(run.session, name or run.actor_name)
    guid = actor["actor_guid"]
    transform = scene.read_transforms(run.session, [guid])["actors"][0]
    position = [a + b for a, b in zip(transform["position"], (dx, dy, dz))]
    check_stop()
    scene.set_actor_transform(run.scene_name, guid, {"archive_session": run.session, "position": position})
