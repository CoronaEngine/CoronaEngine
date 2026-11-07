"""Exercise named project selection and cancellation with this engine's Windows dialog."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import threading
from urllib.request import urlopen

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
    user.GetParent.argtypes = [wintypes.HWND]
    user.GetParent.restype = wintypes.HWND
    user.IsWindow.argtypes = [wintypes.HWND]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.SendMessageW.restype = ctypes.c_ssize_t
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    def label(hwnd, method):
        text = ctypes.create_unicode_buffer(512)
        method(hwnd, text, len(text))
        return text.value

    def dialogs():
        found = []

        @callback
        def visit(hwnd, _):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if (pid.value == engine_pid and user.IsWindowVisible(hwnd)
                    and label(hwnd, user.GetWindowTextW) == '打开 Corona 或外部场景'):
                found.append(hwnd)
            return True
        user.EnumWindows(visit, 0)
        return found

    def dismiss(hwnd, path=None):
        if path is not None:
            edits = []
            reported = False

            @callback
            def visit(child, _):
                if user.IsWindowVisible(child) and label(child, user.GetClassNameW) == 'Edit':
                    edits.append((child, user.GetDlgCtrlID(child),
                                  label(user.GetParent(child), user.GetClassNameW)))
                return True

            def filename_edit():
                nonlocal reported
                edits.clear()
                user.EnumChildWindows(hwnd, visit, 0)
                if not reported:
                    print(json.dumps({'project_filename_edits': edits}), flush=True)
                    reported = True
                matches = [child for child, control, parent in edits
                           if control == 0x480 or parent in ('ComboBox', 'ComboBoxEx32')]
                return matches[0] if len(matches) == 1 else None

            edit = wait(filename_edit, bool, 'project filename edit')
            value = ctypes.create_unicode_buffer(str(path))
            assert user.SendMessageW(edit, 0xC, 0, ctypes.addressof(value))
        assert user.PostMessageW(hwnd, 0x111, 1 if path is not None else 2, 0)
        wait(lambda: bool(user.IsWindow(hwnd)), lambda exists: not exists, 'project dialog closes')

    with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
        targets = [item for item in json.load(response) if item['type'] == 'page'
                   and '/CabbageEditor/Frontend/' in item['url'] and 'uiOwner=' not in item['url']]
    assert len(targets) == 1, targets
    main = Frontend(port, target_id=targets[0]['id'])
    try:
        archive = main.evaluate('corona.scene.readActive()')['archive_session']
        assert main.evaluate('corona.project.openProjectFile(1).catch(e=>e.code)') == 'INVALID_ARGUMENT'
        for selected in [True, False]:
            errors = []

            def drive():
                try:
                    dismiss(wait(dialogs, bool, 'project selection dialog')[0],
                            root / 'scene.ini' if selected else None)
                except Exception as error:
                    errors.append(error)
                    for dialog in dialogs():
                        dismiss(dialog)

            driver = threading.Thread(target=drive, daemon=True)
            driver.start()
            evaluation_error = None
            try:
                result = main.evaluate('corona.project.openProjectFile()')
            except Exception as error:
                evaluation_error = error
            finally:
                driver.join(timeout=35)
            assert not driver.is_alive(), 'Project picker driver did not finish'
            if errors:
                raise errors[0] from evaluation_error
            if evaluation_error:
                raise evaluation_error
            if selected:
                assert set(result) == {'name', 'path'}, result
                assert Path(result['path']).resolve() == (root / 'scene.ini').resolve(), result
                assert result['name'] == 'scene', result
            else:
                assert result == {}, result
            assert main.evaluate('corona.scene.readActive()')['archive_session'] == archive
        return dict(selection=True, cancellation=True, archive_unchanged=True, direct_values=True)
    finally:
        for dialog in dialogs():
            dismiss(dialog)
        main.socket.close()
