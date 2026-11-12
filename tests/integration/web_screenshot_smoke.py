"""Exercise deployed screenshot buttons and their real Windows save dialog."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct
import sys
import threading
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def run(port, root, engine_pid):
    user = ctypes.WinDLL('user32', use_last_error=True)
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback, wintypes.LPARAM]
    user.EnumChildWindows.argtypes = [wintypes.HWND, callback, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.GetDlgCtrlID.argtypes = [wintypes.HWND]
    user.IsWindow.argtypes = [wintypes.HWND]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.SendMessageW.restype = ctypes.c_ssize_t
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    def label(hwnd, method):
        value = ctypes.create_unicode_buffer(512)
        method(hwnd, value, len(value))
        return value.value

    def edit_text(hwnd):
        value = ctypes.create_unicode_buffer(2048)
        user.SendMessageW(hwnd, 0xD, len(value), ctypes.addressof(value))
        return value.value

    def dialogs():
        found = []

        @callback
        def visit(hwnd, _):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value == engine_pid and user.IsWindowVisible(hwnd) and label(hwnd, user.GetWindowTextW) == '保存截图':
                found.append(hwnd)
            return True
        user.EnumWindows(visit, 0)
        return found

    def dismiss(hwnd, path=None):
        if path is not None:
            edits = []

            @callback
            def visit(child, _):
                if label(child, user.GetClassNameW) == 'Edit' and user.IsWindowVisible(child):
                    edits.append((child, user.GetDlgCtrlID(child), edit_text(child)))
                return True
            def filename_edits():
                edits.clear()
                user.EnumChildWindows(hwnd, visit, 0)
                return list(edits)
            wait(filename_edits, lambda values: any(text.startswith('screenshot_') for _, _, text in values),
                 'native screenshot filename initialized')
            print(json.dumps({'filenameEdits': edits}, ensure_ascii=False), flush=True)
            filename = next((child for child, _, text in edits if text.startswith('screenshot_')), None)
            assert filename, f'Native filename edit not found: {edits}'
            value = ctypes.create_unicode_buffer(str(path))
            assert user.SendMessageW(filename, 0xC, 0, ctypes.addressof(value))
            assert edit_text(filename) == str(path)
        assert user.PostMessageW(hwnd, 0x111, 1 if path is not None else 2, 0)
        wait(lambda: bool(user.IsWindow(hwnd)), lambda exists: not exists, 'native screenshot dialog closes')

    def targets():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
            return [target for target in json.load(response) if target['type'] == 'page'
                    and '/CabbageEditor/Frontend/' in target['url']]

    def click_with_dialog(client, expression, path=None):
        errors = []

        def drive_dialog():
            try:
                dismiss(wait(dialogs, bool, 'native screenshot save dialog')[0], path)
            except Exception as error:
                errors.append(error)
        # CEF single-process modal loops can delay the CDP response itself.
        # Drive only this engine's native dialog concurrently with the click.
        driver = threading.Thread(target=drive_dialog, daemon=True)
        driver.start()
        try:
            client.evaluate(expression, wait=False)
        except Exception:
            if errors:
                raise errors[0]
            raise
        finally:
            driver.join(timeout=35)
        assert not driver.is_alive(), 'Native dialog driver did not finish'
        if errors:
            raise errors[0]

    main = Frontend(port, target_id=targets()[0]['id'])
    def cameras():
        return main.evaluate("(async()=>{const s=await corona.scene.readActive();return "
                             "(await corona.scene.readScene({scene:s.scene,archive_session:s.archive_session,cameras:true})).cameras;})()")
    panel = camera_view = None
    window_token = None
    try:
        main.evaluate("localStorage.setItem('corona.activeProjectPath'," + json.dumps(str(root)) + ");location.hash='/'")
        wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'MainPage mount')
        service = """(async()=>{const m=await import(document.querySelector('script[type=module]').src);
          return Object.values(m).find(v=>typeof v?.openNativePanel==='function');})()"""
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda value: value and value.get('enabled'), 'Web owner ready')
        opened = main.evaluate(f"{service}.then(s=>s.openNativePanel({{panelId:'SceneTools',routePath:'/SceneBar',width:700,height:800}}))")
        window_token = opened['window_token']
        target = wait(lambda: next((target for target in targets() if '#/SceneBar?' in target['url']), None), bool, 'SceneBar document')
        panel = Frontend(port, target_id=target['id'])
        wait(lambda: panel.evaluate("!!document.querySelector('[data-actor-name=Probe]')"), bool, 'SceneBar ready')
        monitor = """(()=>{if(typeof corona.editor!=='undefined')throw Error('Generic native dispatch remains');return true;})()"""
        panel.evaluate(monitor)
        initial = cameras()
        output = root.parent / 'scene-ui-screenshot.png'
        click_with_dialog(panel, "document.querySelector('button[title=截图]').click();true", output)
        wait(output.exists, bool, 'SceneBar PNG')
        png = output.read_bytes()
        assert png[:8] == b'\x89PNG\r\n\x1a\n'
        dimensions = struct.unpack('>II', png[16:24])
        assert dimensions == (initial[0]['width'], initial[0]['height']), dimensions
        click_with_dialog(panel, "document.querySelector('button[title=截图]').click();true")
        secondary = next(camera for camera in initial if camera['camera_id'] == 'focus-camera')
        camera_snapshot = {**secondary, 'scene_id': 'scene.ini'}
        main.evaluate(f'{service}.then(s=>s.createCameraView({json.dumps(camera_snapshot)}))')
        camera_target = wait(lambda: next((target for target in targets() if '#/CameraView?' in target['url']
                                         and 'camera=focus-camera' in target['url']), None), bool, 'CameraView document')
        camera_view = Frontend(port, target_id=camera_target['id'])
        wait(lambda: camera_view.evaluate("[...document.querySelectorAll('button')].some(b=>b.textContent.trim()==='Screenshot')"), bool, 'CameraView ready')
        viewport_size = camera_view.evaluate('({width:Math.max(Math.round(innerWidth),64),height:Math.max(Math.round(innerHeight),64)})')
        secondary = wait(lambda: next(camera for camera in cameras()
                                      if camera['camera_id'] == 'focus-camera'),
                         lambda camera: all(camera[key] == viewport_size[key] for key in ['width', 'height']),
                         'CameraView initial native size synchronization')
        camera_view.evaluate(monitor)
        secondary_output = root.parent / 'camera-ui-screenshot.png'
        click_with_dialog(camera_view, "[...document.querySelectorAll('button')].find(b=>b.textContent.trim()==='Screenshot').click();true", secondary_output)
        wait(secondary_output.exists, bool, 'CameraView PNG')
        secondary_png = secondary_output.read_bytes()
        assert secondary_png[:8] == b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>II', secondary_png[16:24]) == (secondary['width'], secondary['height'])
        assert camera_view.evaluate("document.querySelector('.error')?.textContent || ''") == ''
        assert panel.evaluate('typeof corona.editor') == 'undefined'
        assert camera_view.evaluate('typeof corona.editor') == 'undefined'
        after = cameras()
        for before in initial:
            current = next(camera for camera in after if camera['camera_id'] == before['camera_id'])
            for field in ['position', 'forward', 'world_up', 'fov', 'output_mode']:
                assert current[field] == before[field], (field, before, current)
        current_secondary = next(camera for camera in after if camera['camera_id'] == 'focus-camera')
        for field in ['width', 'height']:
            assert current_secondary[field] == secondary[field]
        return {'documents': 3, 'nativeDialogs': 3, 'cancellations': 1, 'pngFiles': [str(output), str(secondary_output)],
                'mainDimensions': dimensions, 'retiredScreenshotRequests': 0, 'cameraPoseAndOutputUnchanged': True}
    finally:
        for dialog in dialogs():
            dismiss(dialog)
        if camera_view:
            main.evaluate("corona.window.close({sceneId:'scene.ini',cameraId:'focus-camera'})")
            camera_view.socket.close()
        if window_token is not None:
            main.evaluate("corona.window.close({panelId:'SceneTools'})")
        if panel:
            panel.socket.close()
        main.socket.close()
