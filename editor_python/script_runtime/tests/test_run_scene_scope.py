from Frontend.src.blockly.generators.runtime import block_program as ce
import threading

import pytest
from script_runtime import file_api


def test_same_scene_is_shared_only_with_children_of_the_same_run():
    a = ce.bind_context(ce.create_context("root-a", "actor", "scene.ini", "Probe"))
    try:
        ce.data_set("SCENE", "points", 37)
        ce._shared_lists(a)["items"] = [1]
        ce._shared_object_tags(a)["Probe"] = "old"
        child = ce.bind_context(ce.create_context("child", "actor", "scene.ini", "Clone", parent=a))
        try:
            assert ce.data_get("SCENE", "points") == 37
            ce.data_set("SCENE", "points", 42)
            assert ce._shared_variables(a)["points"] == 42
        finally:
            ce.release_context(child)
    finally:
        ce.release_context(a)
    b = ce.bind_context(ce.create_context("root-b", "actor", "scene.ini", "Probe"))
    try:
        assert ce.data_get("SCENE", "points", "unset") == "unset"
        assert not ce._shared_lists(b) and not ce._shared_object_tags(b)
    finally:
        ce.release_context(b)


def test_file_failure_stops_an_active_broadcast_worker():
    started, finished = threading.Event(), threading.Event()

    def handler():
        started.set()
        try:
            while not finished.wait(.01):
                ce.assert_engine_operation_allowed()
        finally:
            finished.set()

    run = file_api.FileRun("failure", "scope", "actor", "scene.ini", "Probe")
    try:
        with pytest.raises(ValueError), file_api.bind(run):
            ce.register_broadcast_handler("work", handler)
            ce.broadcast("work")
            assert started.wait(2)
            raise ValueError("file failed")
        assert finished.wait(2)
    finally:
        finished.set()
