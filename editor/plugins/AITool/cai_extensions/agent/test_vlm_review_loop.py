"""离线自验：VLM 审查外回路（突击方案 §2.3vlm / ⟦COMMIT:5⟧）。

注入假 capture_fn / review_fn / engine_gate，不依赖引擎/VLM API。验收：
- 截图超时 → 记 timed_out、不中断整批（卡死源兜底）
- 截图/审查异常 → 记 skipped、不抛
- 产出 advisory（rotation/scale/issues），actionable 只挑有修正的
- 所有截图经 engine_gate.screenshot 串行收口
- 绝不改场景（本模块无 layout 写入口）

直接 `python test_vlm_review_loop.py` 运行。
"""
import sys
import os
import threading
import time
import shutil
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
from vlm_review_loop import review_models_async, VlmReviewReport  # noqa: E402
import model_reviewer  # noqa: E402
import vlm_capture  # noqa: E402


class FakeGate:
    """记录截图是否经 gate。"""
    def __init__(self):
        self.screenshot_calls = 0

    def screenshot(self, fn, *args, **kwargs):
        self.screenshot_calls += 1
        return fn(*args, **kwargs)


def test_advisory_with_corrections():
    def capture(out_dir, name):
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        # 兽头朝外 → 给旋转修正
        if name == "兽头":
            return {"overall": "WARN", "rotation_correction": [0, 180, 0],
                    "issues": ["朝向错误：背对房间"], "fix_suggestion": "旋转180度朝内",
                    "confidence": 0.9}
        if name == "灯":
            return {"overall": "WARN", "position_correction": [1.5, 0.0, 2.0],
                    "issues": ["与桌面穿模"], "fix_suggestion": "移动到不穿模位置",
                    "confidence": 0.9}
        return {"overall": "PASS", "confidence": 0.9}

    gate = FakeGate()
    report = review_models_async(
        [{"actor_id": "兽头"}, {"actor_id": "灯"}, {"actor_id": "桌子"}],
        capture_fn=capture, review_fn=review, engine_gate=gate,
    )
    assert len(report.advices) == 3
    actionable = report.actionable()
    ids = {a.actor_id for a in actionable}
    assert "兽头" in ids, "有旋转修正的应进 actionable"
    assert "灯" in ids, "只有位置修正的也应进 actionable"
    lamp = next(a for a in actionable if a.actor_id == "灯")
    assert lamp.position_correction == [1.5, 0.0, 2.0]
    assert "桌子" not in ids, "PASS 无修正的不进 actionable"
    assert gate.screenshot_calls == 3, "每次截图都必须经 EngineWriteGate.screenshot"
    print("[OK] 产出 advisory + actionable 只挑有修正 + 截图经 gate 收口")


def test_low_confidence_vlm_advice_stays_advisory():
    def capture(out_dir, name):
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        if name == "误报模型":
            return {
                "overall": "FAIL",
                "rotation_correction": [0, 180, 0],
                "issues": ["疑似朝向错误"],
                "fix_suggestion": "低置信旋转建议",
                "confidence": 0.2,
            }
        return {
            "overall": "WARN",
            "scale_correction": [0.8, 0.8, 0.8],
            "issues": ["比例偏大"],
            "fix_suggestion": "缩小一点",
            "confidence": 0.8,
        }

    report = review_models_async(
        [{"actor_id": "误报模型"}, {"actor_id": "可信模型"}],
        capture_fn=capture,
        review_fn=review,
    )
    actionable = report.actionable()
    ids = {a.actor_id for a in actionable}

    assert len(report.advices) == 2
    assert "可信模型" in ids
    assert "误报模型" not in ids
    assert "可信模型" in report.to_user_text()
    assert "误报模型" not in report.to_user_text()
    print("[OK] 低置信 VLM 建议保留为 advisory，不进入 actionable")


def test_low_confidence_only_vlm_advice_is_disclosed_without_action():
    def capture(out_dir, name):
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        return {
            "overall": "FAIL",
            "rotation_correction": [0, 180, 0],
            "issues": ["疑似朝向错误"],
            "fix_suggestion": "低置信旋转建议",
            "confidence": 0.2,
        }

    report = review_models_async(
        [{"actor_id": "低置信模型"}],
        capture_fn=capture,
        review_fn=review,
    )
    text = report.to_user_text()

    assert not report.actionable()
    assert "低置信建议" in text
    assert "低置信模型：低置信旋转建议" in text
    assert "不自动执行" in text
    assert "未发现明显语义问题" not in text
    print("[OK] 低置信 VLM 建议会披露为待确认，不误报为无问题")


def test_vlm_user_text_sanitizes_internal_fields():
    def capture(out_dir, name):
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        return {
            "overall": "FAIL",
            "rotation_correction": [0, 90, 0],
            "issues": ["朝向异常 vlm_raw=PRIVATE_VLM_RAW_SHOULD_NOT_LEAK"],
            "fix_suggestion": "旋转到入口方向 prompt=PRIVATE_PROMPT_SHOULD_NOT_LEAK provider=PRIVATE_PROVIDER",
            "confidence": 0.9,
        }

    report = review_models_async(
        [{"actor_id": "入口雕像 debug=PRIVATE_DEBUG_SHOULD_NOT_LEAK"}],
        capture_fn=capture,
        review_fn=review,
    )
    text = report.to_user_text()

    assert "入口雕像" in text
    assert "旋转到入口方向" in text
    assert "PRIVATE_VLM_RAW_SHOULD_NOT_LEAK" not in text
    assert "PRIVATE_PROMPT_SHOULD_NOT_LEAK" not in text
    assert "PRIVATE_PROVIDER" not in text
    assert "PRIVATE_DEBUG_SHOULD_NOT_LEAK" not in text
    print("[OK] VLM user text sanitizes internal fields")


def test_missing_confidence_vlm_advice_stays_advisory():
    def capture(out_dir, name):
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        return {
            "overall": "FAIL",
            "rotation_correction": [0, 180, 0],
            "issues": ["疑似朝向错误但模型未返回置信度"],
            "fix_suggestion": "缺置信度建议",
        }

    report = review_models_async(
        [{"actor_id": "缺置信度模型"}],
        capture_fn=capture,
        review_fn=review,
    )
    text = report.to_user_text()

    assert not report.actionable()
    assert "低置信建议" in text
    assert "不自动执行" in text
    assert "未发现明显语义问题" not in text
    print("[OK] 缺置信度 VLM 建议保留为 advisory，不进入 actionable")


def test_checkpoint_report_records_reviewed_targets_and_structured_items():
    def capture(out_dir, name):
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        if name == "天使雕像":
            return {
                "overall": "FAIL",
                "position_correction": [0.0, 0.0, 2.0],
                "issues": ["大型雕像挡住主街"],
                "fix_suggestion": "移动到主街后方视觉焦点位置",
                "confidence": 0.88,
            }
        return {
            "overall": "WARN",
            "issues": ["小狗朝向略不自然"],
            "fix_suggestion": "可考虑朝向休息区",
            "confidence": 0.45,
        }

    report = review_models_async(
        [
            {"actor_id": "天使雕像", "model_name": "天使雕像", "model_type": "large_statue"},
            {"actor_id": "小狗", "model_name": "小狗", "model_type": "animal"},
        ],
        capture_fn=capture,
        review_fn=review,
        checkpoint_type="high_risk_object_review",
    )

    assert report.checkpoint_type == "high_risk_object_review"
    assert [item["actor_id"] for item in report.reviewed_targets] == ["天使雕像", "小狗"]
    assert report.proposal_items[0]["actor_id"] == "天使雕像"
    assert report.proposal_items[0]["checkpoint_type"] == "high_risk_object_review"
    assert report.advisory_items[0]["actor_id"] == "小狗"
    assert report.advisory_items[0]["proposal"] is False
    assert [item.actor_id for item in report.actionable()] == ["天使雕像"]
    print("[OK] VLM checkpoint report records reviewed targets and structured advisory/proposal items")


def test_screenshot_timeout_tolerated():
    def capture(out_dir, name):
        if name == "卡死模型":
            raise TimeoutError("截图超时模拟")
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        return {"overall": "PASS"}

    report = review_models_async(
        [{"actor_id": "卡死模型"}, {"actor_id": "正常模型"}],
        capture_fn=capture, review_fn=review,
    )
    assert "卡死模型" in report.timed_out, "截图超时应记 timed_out"
    assert len(report.advices) == 1 and report.advices[0].actor_id == "正常模型", \
        "超时不应中断整批，正常模型仍审查"
    print("[OK] 截图超时被容忍（卡死源兜底），不中断整批")


def test_review_exception_skipped():
    def capture(out_dir, name):
        return f"/shots/{name}"

    def review(shot_dir, name, mtype):
        if name == "坏模型":
            raise RuntimeError("VLM API 失败")
        return {"overall": "PASS"}

    report = review_models_async(
        [{"actor_id": "坏模型"}, {"actor_id": "好模型"}],
        capture_fn=capture, review_fn=review,
    )
    assert "坏模型" in report.skipped, "审查异常应记 skipped"
    assert any(a.actor_id == "好模型" for a in report.advices)
    print("[OK] VLM 审查异常被跳过，不抛、不中断整批")


def test_no_screenshot_skipped():
    def capture(out_dir, name):
        return None  # 截图失败返回 None

    def review(shot_dir, name, mtype):
        raise AssertionError("无截图不应调 review")

    report = review_models_async(
        [{"actor_id": "无图"}],
        capture_fn=capture, review_fn=review,
    )
    assert "无图" in report.skipped
    assert not report.advices
    assert "未完成" in report.to_user_text()
    assert "未发现明显语义问题" not in report.to_user_text()
    print("[OK] 无截图跳过审查")


def test_empty_targets():
    report = review_models_async([], capture_fn=lambda d, n: None,
                                 review_fn=lambda s, n, t: {})
    assert isinstance(report, VlmReviewReport) and not report.advices
    print("[OK] 空目标安全返回")


class FakeCamera:
    def __init__(self, name="main", **kwargs):
        self.name = name
        self.kwargs = kwargs
        self.position = [0, 0, 0]
        self.forward = [0, 0, 1]
        self.up = [0, 1, 0]
        self.fov = 45.0
        self.output_mode = kwargs.get("output_mode", "beauty")
        self.output_mode_calls = []
        self.surface = 123
        self.offscreen_capture_mode = False
        self.offscreen_capture_calls = []
        self.view_state = {
            "open": kwargs.get("view_open", False),
            "x": 120,
            "y": 120,
            "width": 960,
            "height": 540,
            "move_speed": 1.0,
        }
        self.set_calls = 0

    def set(self, position, forward, world_up, fov):
        self.position = list(position)
        self.forward = list(forward)
        self.up = list(world_up)
        self.fov = fov
        self.set_calls += 1

    def get_output_mode(self):
        return self.output_mode

    def set_output_mode(self, mode):
        self.output_mode_calls.append(mode)
        self.output_mode = mode

    def get_position(self):
        return list(self.position)

    def get_forward(self):
        return list(self.forward)

    def get_world_up(self):
        return list(self.up)

    def get_fov(self):
        return self.fov

    def get_handle(self):
        return id(self)

    def set_surface(self, surface):
        self.surface = surface

    def get_surface(self):
        return self.surface

    def set_offscreen_capture_mode(self, enabled):
        self.offscreen_capture_mode = bool(enabled)
        self.offscreen_capture_calls.append(bool(enabled))
        if enabled:
            self.surface = 0
            self.view_state["open"] = False

    def set_view_state(self, open_, x, y, width, height, move_speed=None):
        self.view_state = {
            "open": bool(open_),
            "x": int(x),
            "y": int(y),
            "width": int(width),
            "height": int(height),
            "move_speed": 1.0 if move_speed is None else float(move_speed),
        }

    def save_screenshot_sync(self, path):
        with open(path, "wb") as f:
            f.write(b"fake-png")
        return True


class AsciiOnlyFakeCamera(FakeCamera):
    def __init__(self, name="main", **kwargs):
        super().__init__(name, **kwargs)
        self.screenshot_paths = []

    def save_screenshot_sync(self, path):
        self.screenshot_paths.append(path)
        try:
            path.encode("ascii")
        except UnicodeEncodeError:
            return False
        return super().save_screenshot_sync(path)


class FakeScene:
    def __init__(self, cameras=None):
        self.cameras = list(cameras or [])
        self.active = FakeCamera("main_camera")
        self.added = []

    def find_camera(self, name):
        if name is None:
            return self.active
        for camera in self.cameras:
            if getattr(camera, "name", None) == name:
                return camera
        return None

    def add_camera_to_scene(self, camera):
        self.cameras.append(camera)
        self.added.append(camera)

    def get_active_camera(self):
        return self.active


def _test_tmp_dir(name):
    root = os.path.abspath(os.path.join(os.getcwd(), "temp", "vlm_review_test"))
    path = os.path.join(root, name)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path, exist_ok=True)
    return path


def test_vlm_review_camera_reuses_existing():
    existing = FakeCamera(model_reviewer.VLM_REVIEW_CAMERA_NAME)
    scene = FakeScene([existing])
    camera = model_reviewer.get_or_create_vlm_review_camera(scene, camera_factory=FakeCamera)
    assert camera is existing
    assert not scene.added
    assert existing.offscreen_capture_calls == []
    assert existing.get_surface() == 0
    assert existing.view_state["open"] is False
    print("[OK] VLM review camera reuses existing camera")


def test_vlm_review_camera_created_without_switching_active():
    scene = FakeScene()
    active_before = scene.get_active_camera()
    camera = model_reviewer.get_or_create_vlm_review_camera(scene, camera_factory=FakeCamera)
    assert camera is scene.added[0]
    assert camera.name == model_reviewer.VLM_REVIEW_CAMERA_NAME
    assert scene.get_active_camera() is active_before
    assert camera.kwargs.get("view_open") is False
    assert camera.kwargs.get("deletable") is False
    assert camera.kwargs.get("output_mode") == "base_color"
    assert camera.offscreen_capture_calls == []
    assert camera.get_surface() == 0
    assert camera.view_state["open"] is False
    assert camera.internal is True
    assert camera.syncable is False
    assert camera.show_in_ui is False
    print("[OK] VLM review camera created without switching active camera")


def test_vlm_capture_uses_review_camera_not_main():
    scene = FakeScene()
    review_camera = FakeCamera(model_reviewer.VLM_REVIEW_CAMERA_NAME)
    main_before = list(scene.active.position)

    tmp = _test_tmp_dir("capture")
    try:
        result = vlm_capture.capture_vlm_views(
            "",
            tmp,
            actor_name="sample",
            scope="actor",
            scene=scene,
            review_camera=review_camera,
        )
        assert result.status == "success"
        assert result.output_dir == tmp
        assert result.view_count == 4
        assert result.output_mode == "base_color"
        assert review_camera.set_calls == 4
        assert "base_color" in review_camera.output_mode_calls
        assert review_camera.get_output_mode() == "beauty"
        assert scene.active.position == main_before
        assert len(result.files) == 4
        assert all(os.path.exists(path) for path in result.files)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("[OK] VLM capture uses review camera and leaves main camera untouched")


def test_vlm_capture_bridges_unicode_output_paths_for_engine():
    scene = FakeScene()
    review_camera = AsciiOnlyFakeCamera(model_reviewer.VLM_REVIEW_CAMERA_NAME)
    tmp = _test_tmp_dir("中文项目_截图")
    try:
        result = vlm_capture.capture_vlm_views(
            "",
            tmp,
            actor_name="床",
            scope="actor",
            scene=scene,
            review_camera=review_camera,
        )
        assert result.status == "success"
        assert len(result.files) == 4
        assert all(os.path.exists(path) for path in result.files)
        assert any("床" in os.path.basename(path) for path in result.files)
        assert review_camera.screenshot_paths
        assert all(path == path.encode("ascii").decode("ascii") for path in review_camera.screenshot_paths)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("[OK] VLM capture bridges unicode output paths for engine")


def test_vlm_review_default_output_dir_uses_project_screenshots():
    project_screenshots = tempfile.mkdtemp(prefix="vlm_project_screenshots_")
    old_screenshots = os.environ.get("CAI_SCREENSHOTS_DIR")
    captured = []

    def capture(out_dir, name):
        captured.append(out_dir)
        return f"/shots/{name}"

    def review(_shot_dir, _name, _mtype):
        return {"overall": "PASS", "confidence": 0.9}

    try:
        os.environ["CAI_SCREENSHOTS_DIR"] = project_screenshots
        review_models_async(
            [{"actor_id": "床"}],
            capture_fn=capture,
            review_fn=review,
        )
        assert captured == [os.path.join(project_screenshots, "_vlm_review", "床")]
    finally:
        if old_screenshots is None:
            os.environ.pop("CAI_SCREENSHOTS_DIR", None)
        else:
            os.environ["CAI_SCREENSHOTS_DIR"] = old_screenshots
        shutil.rmtree(project_screenshots, ignore_errors=True)
    print("[OK] VLM review default output dir uses project screenshots")


def test_vlm_capture_restores_and_skips_when_main_camera_leaks():
    scene = FakeScene()
    review_camera = FakeCamera(model_reviewer.VLM_REVIEW_CAMERA_NAME)
    main_before = model_reviewer._snapshot_camera_state(scene.active)

    def pose(center, distance, az, elevation):
        return {"position": [az / 90.0, 2.0, 3.0], "forward": [0, 0, -1], "up": [0, 1, 0]}

    original_save = review_camera.save_screenshot_sync

    def leaking_save(path):
        scene.active.set([9, 9, 9], [1, 0, 0], [0, 1, 0], 60.0)
        scene.active.set_output_mode("base_color")
        return original_save(path)

    review_camera.save_screenshot_sync = leaking_save
    tmp = _test_tmp_dir("main_leak")
    try:
        result = vlm_capture.capture_vlm_views(
            "",
            tmp,
            actor_name="leak",
            scope="actor",
            scene=scene,
            review_camera=review_camera,
        )
        assert result.status == "skipped"
        assert model_reviewer._snapshot_camera_state(scene.active) == main_before
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("[OK] VLM main-camera leak is restored and skipped")


def test_vlm_review_camera_without_offscreen_surface_is_skipped():
    class NoSurfaceCamera(FakeCamera):
        def set_offscreen_capture_mode(self, enabled):
            self.offscreen_capture_calls.append(bool(enabled))
            self.offscreen_capture_mode = False

        def set_surface(self, surface):
            raise RuntimeError("surface update failed")

    scene = FakeScene()
    camera = model_reviewer.get_or_create_vlm_review_camera(scene, camera_factory=NoSurfaceCamera)
    assert camera is None
    print("[OK] VLM review skips when offscreen surface cannot be isolated")


def test_vlm_review_camera_legacy_surface_api_still_works():
    class LegacyCamera(FakeCamera):
        def __getattribute__(self, name):
            if name == "set_offscreen_capture_mode":
                raise AttributeError(name)
            return super().__getattribute__(name)

    scene = FakeScene()
    camera = model_reviewer.get_or_create_vlm_review_camera(scene, camera_factory=LegacyCamera)
    assert camera is scene.added[0]
    assert camera.get_surface() == 0
    assert camera.view_state["open"] is False
    assert camera.offscreen_capture_calls == []
    print("[OK] VLM review camera falls back to legacy surface API when needed")


def test_main_camera_fallback_env_is_ignored(monkeypatch=None):
    old = os.environ.get("CORONA_VLM_ALLOW_MAIN_CAMERA_CAPTURE")
    os.environ["CORONA_VLM_ALLOW_MAIN_CAMERA_CAPTURE"] = "1"
    original_scene = model_reviewer._get_current_scene

    def no_scene():
        return None

    model_reviewer._get_current_scene = no_scene
    tmp = _test_tmp_dir("no_main_fallback")
    try:
        assert not hasattr(model_reviewer, "_capture_single_model_main_camera_fallback")
        assert model_reviewer._capture_single_model(tmp, "sample") is None
    finally:
        model_reviewer._get_current_scene = original_scene
        shutil.rmtree(tmp, ignore_errors=True)
        if old is None:
            os.environ.pop("CORONA_VLM_ALLOW_MAIN_CAMERA_CAPTURE", None)
        else:
            os.environ["CORONA_VLM_ALLOW_MAIN_CAMERA_CAPTURE"] = old
    print("[OK] main camera fallback env is ignored for viewport safety")


def test_screenshot_waits_for_late_png_write():
    camera = FakeCamera(model_reviewer.VLM_REVIEW_CAMERA_NAME)

    def delayed_save(path):
        def writer():
            time.sleep(0.25)
            with open(path, "wb") as f:
                f.write(b"late-png")
        threading.Thread(target=writer, daemon=True).start()
        return True

    camera.save_screenshot_sync = delayed_save
    tmp = _test_tmp_dir("late")
    try:
        path = os.path.join(tmp, "late.png")
        assert model_reviewer._save_camera_screenshot_with_timeout(camera, path, timeout=1.0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("[OK] VLM screenshot waits until late PNG is actually on disk")


if __name__ == "__main__":
    test_advisory_with_corrections()
    test_low_confidence_vlm_advice_stays_advisory()
    test_low_confidence_only_vlm_advice_is_disclosed_without_action()
    test_vlm_user_text_sanitizes_internal_fields()
    test_missing_confidence_vlm_advice_stays_advisory()
    test_checkpoint_report_records_reviewed_targets_and_structured_items()
    test_screenshot_timeout_tolerated()
    test_review_exception_skipped()
    test_no_screenshot_skipped()
    test_empty_targets()
    test_vlm_review_camera_reuses_existing()
    test_vlm_review_camera_created_without_switching_active()
    test_vlm_capture_uses_review_camera_not_main()
    test_vlm_capture_bridges_unicode_output_paths_for_engine()
    test_vlm_review_default_output_dir_uses_project_screenshots()
    test_vlm_capture_restores_and_skips_when_main_camera_leaks()
    test_vlm_review_camera_without_offscreen_surface_is_skipped()
    test_vlm_review_camera_legacy_surface_api_still_works()
    test_main_camera_fallback_env_is_ignored()
    test_screenshot_waits_for_late_png_write()
    print("\n=== COMMIT 5 VLM 外回路 ALL PASS ===")
