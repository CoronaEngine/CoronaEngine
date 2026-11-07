"""Exercise every registered packaged Vue panel in the existing native Dock."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
from urllib.request import urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def minimize_restore(engine_pid, main, child, targets, service):
    """Minimize only this launch's real HWNDs, then verify both CEF documents survive."""
    user = ctypes.WinDLL('user32', use_last_error=True)
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.IsIconic.argtypes = [wintypes.HWND]
    user.ShowWindowAsync.argtypes = [wintypes.HWND, ctypes.c_int]
    windows = []

    @callback
    def visit(hwnd, _):
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == engine_pid and user.IsWindowVisible(hwnd):
            windows.append(hwnd)
        return True

    assert user.EnumWindows(visit, 0)
    assert len(windows) >= 2, f'Expected main and detached panel HWNDs: {windows}'
    before_targets = sorted(target['id'] for target in targets())
    before = []
    for client in (main, child):
        before.append(client.evaluate("(async()=>({token:window.__minimizeProbe=crypto.randomUUID(),"
                                      "tab:await corona.window.getCurrent(),scene:await corona.scene.readActive()}))()"))
    try:
        for hwnd in windows:
            assert user.ShowWindowAsync(hwnd, 6), f'Minimize HWND failed: {hwnd}'
        wait(lambda: [bool(user.IsIconic(hwnd)) for hwnd in windows], all, 'native windows minimized')
    finally:
        for hwnd in windows:
            user.ShowWindowAsync(hwnd, 9)
        wait(lambda: [bool(user.IsIconic(hwnd)) for hwnd in windows],
             lambda states: not any(states), 'native windows restored')
    assert sorted(target['id'] for target in targets()) == before_targets, 'Minimize recreated a CEF document'
    for client, previous in zip((main, child), before):
        after = client.evaluate("(async()=>({token:window.__minimizeProbe,tab:await corona.window.getCurrent(),"
                                "scene:await corona.scene.readActive()}))()")
        assert after['token'] == previous['token'], (previous, after)
        assert after['tab']['tabId'] == previous['tab']['tabId'], (previous, after)
        assert after['scene']['archive_session'] == previous['scene']['archive_session'], (previous, after)
    policy = child.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())')
    assert policy and policy.get('enabled'), policy
    return dict(native_windows=len(windows), documents_preserved=True, archive_preserved=True,
                peer_owner_request_after_restore=True)


def run(port, root, engine_pid):
    repository = Path(__file__).resolve().parents[2]
    manifest = repository / 'CoronaEditor/editor/Frontend/src/config/pluginManifest.js'
    # This registry is declarative. Match every entry rather than silently
    # limiting acceptance to the subset that happened to be tested previously.
    panels = re.findall(r"id:\s*'([^']+)'\s*,\s*routePath:\s*'([^']+)'", manifest.read_text(encoding='utf-8'))
    selectors = {
        'SceneControls': '.scene-controls-body', 'CabbageTasks': '.cabbage-tasks-panel',
        'SceneTools': '.scene-tools-panel', 'LightFieldCalibration': '.lfd-calib input',
        'Object': '.object-panel-shell', 'AITool': 'img', 'FileManager': '.custom-scrollbar',
        'ProjectSettings': 'input', 'NodeGraphPanel': '.node-graph-panel .blocklySvg',
        'CabbageChatPanel': '.cabbage-chat-shell', 'EditorSettings': '.esc-panel',
    }
    assert len(panels) == len(selectors) and {panel for panel, _ in panels} == selectors.keys(), panels

    def targets():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
            return [value for value in json.load(response) if value['type'] == 'page'
                    and '/CabbageEditor/Frontend/' in value['url']]

    main_targets = [value for value in targets() if 'uiOwner=' not in value['url'] and 'standalone=1' not in value['url']]
    assert len(main_targets) == 1, main_targets
    main = Frontend(port, target_id=main_targets[0]['id'])
    service = """(async()=>{const module=await import(document.querySelector('script[type=module]').src);
      return Object.values(module).find(value=>typeof value?.openNativePanel==='function');})()"""
    # Drive the same Pinia intent as MainPage buttons. openNativePanel is a
    # low-level create operation and intentionally does not deduplicate callers.
    store = "document.querySelector('#app').__vue_app__.config.globalProperties.$pinia._s.get('dock')"
    results = []
    opened = None
    child = None
    try:
        main.evaluate("location.hash='/'")
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda value: value and value.get('enabled'), 'panel owner ready')
        wait(lambda: main.evaluate(f'!!{store}'), bool, 'production Dock owner')
        archive = main.evaluate('corona.scene.readActive()')['archive_session']
        assert archive
        for panel_id, route in panels:
            print(f'packaged panel {panel_id}', flush=True)
            opened = dict(panel_id=panel_id)
            main.evaluate(f'{store}.openPanel({json.dumps(panel_id)})')
            target = wait(lambda: next((value for value in targets()
                                       if f'#{route}?' in value['url'] and 'uiOwner=' in value['url']), None),
                          bool, f'{panel_id} document')
            child = Frontend(port, target_id=target['id'])
            child.call('Runtime.enable')
            wait(lambda: child.evaluate(f'!!document.querySelector({json.dumps(selectors[panel_id])})'),
                 bool, f'{panel_id} mounted')
            identity = child.evaluate('corona.window.getCurrent()')
            opened['tab_id'] = identity['tabId']
            assert opened['tab_id'] > 0, identity
            peer_scene = child.evaluate('corona.scene.readActive()')
            assert peer_scene['archive_session'] == archive and peer_scene['scene'] == 'scene.ini', peer_scene
            file_crud = False
            if panel_id == 'FileManager':
                wait(lambda: child.evaluate("document.querySelectorAll('.tree-node').length"), bool, 'file tree ready')
                def context(name=None):
                    child.evaluate("(()=>{const name=" + json.dumps(name) + ";const row=name===null?document.querySelector('.custom-scrollbar'):"
                                   "[...document.querySelectorAll('.tree-node > .group')].find(r=>r.querySelector('.truncate')?.textContent===name);"
                                   "if(!row)throw Error('file row missing '+name);row.dispatchEvent(new MouseEvent('contextmenu',"
                                   "{bubbles:true,cancelable:true,clientX:30,clientY:50}));})()")
                    wait(lambda: child.evaluate("!!document.querySelector('[data-file-context-menu]')"), bool, 'file context menu')
                def action(index):
                    child.evaluate(f"document.querySelector('[data-file-context-menu]').children[{index}].click()")
                def confirm_name(name):
                    wait(lambda: child.evaluate("!!document.querySelector('[data-file-dialog] input')"), bool, 'file dialog')
                    child.evaluate("(()=>{const input=document.querySelector('[data-file-dialog] input');input.value=" + json.dumps(name)
                                   + ";input.dispatchEvent(new Event('input',{bubbles:true}));document.querySelectorAll('[data-file-dialog] button')[1].click();})()")
                    wait(lambda: child.evaluate("!!document.querySelector('[data-file-dialog]')"), lambda value: not value, 'file dialog committed')
                def has_name(name):
                    return child.evaluate("[...document.querySelectorAll('.tree-node .truncate')].some(n=>n.textContent===" + json.dumps(name) + ")")
                context(); action(0); confirm_name('UI files')
                wait(lambda: has_name('UI files'), bool, 'Vue created folder')
                context('UI files'); action(1); confirm_name('UI Actor')
                wait(lambda: has_name('UI Actor.actor'), bool, 'Vue created Actor template')
                context('UI Actor.actor'); action(2); confirm_name('Renamed.actor')
                wait(lambda: has_name('Renamed.actor'), bool, 'Vue renamed Actor template')
                context('Renamed.actor'); action(0)
                wait(lambda: child.evaluate("!document.querySelector('[data-file-context-menu]')"), bool, 'Vue opened Actor template')
                child.evaluate('window.confirm=()=>true')
                context('Renamed.actor'); action(3)
                wait(lambda: has_name('Renamed.actor'), lambda value: not value, 'Vue deleted Actor template')
                context('UI files'); action(4)
                wait(lambda: has_name('UI files'), lambda value: not value, 'Vue deleted folder')
                file_crud = True
            settings_saved = False
            if panel_id == 'ProjectSettings':
                original = child.evaluate('corona.projectSettings.getActiveProjectInfo()')
                name = original['name'] + ' UI save'
                child.evaluate("(()=>{const input=document.querySelector('input');input.value=" + json.dumps(name)
                               + ";input.dispatchEvent(new Event('input',{bubbles:true}));"
                                 "document.querySelectorAll('button')[1].click();})()")
                wait(lambda: child.evaluate('corona.projectSettings.getActiveProjectInfo()'),
                     lambda value: value['name'] == name, 'settings UI save')
                wait(lambda: child.evaluate("!document.querySelectorAll('button')[1].disabled"), bool, 'settings save settled')
                child.evaluate("(()=>{const input=document.querySelector('input');input.value='unsaved';"
                               "input.dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('button').click();})()")
                wait(lambda: child.evaluate("document.querySelector('input')?.value"),
                     lambda value: value == name, 'settings UI reset')
                child.evaluate('corona.projectSettings.saveActiveProjectInfo(' + json.dumps({
                    'archive_session': original['archive_session'], 'project_path': original['project_path'],
                    'name': original['name'], 'core_version': original['core_version']}) + ')')
                settings_saved = True
            main.evaluate(f'{store}.openPanel({json.dumps(panel_id)})')
            # Wait for Vue's watcher/microtask flush before checking identity.
            main.evaluate("new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))")
            matches = [value for value in targets() if f'#{route}?' in value['url'] and 'uiOwner=' in value['url']]
            assert [value['id'] for value in matches] == [target['id']], matches
            assert child.evaluate('corona.window.getCurrent()')['tabId'] == opened['tab_id']
            assert main.evaluate('corona.scene.readActive()')['archive_session'] == archive
            # Exercise the existing native state machine, with the same CEF
            # document remaining mounted throughout the surface transitions.
            main.evaluate(f'{service}.then(s=>s.redockPanel({{tabId:{opened["tab_id"]}}}))')
            wait(lambda: child.evaluate('corona.window.getCurrent()'),
                 lambda value: value['dockState'] == 'docked', f'{panel_id} docked')
            main.evaluate(f'{service}.then(s=>s.detachPanel({{tabId:{opened["tab_id"]},x:120,y:120,width:900,height:800}}))')
            wait(lambda: child.evaluate('corona.window.getCurrent()'),
                 lambda value: value['dockState'] == 'detached', f'{panel_id} detached')
            assert child.evaluate(f'!!document.querySelector({json.dumps(selectors[panel_id])})')
            restored = minimize_restore(engine_pid, main, child, targets, service) if panel_id == 'FileManager' else None
            exceptions = [event['params'] for event in child.events if event.get('method') == 'Runtime.exceptionThrown']
            assert not exceptions, (panel_id, exceptions)
            main.evaluate(f'{store}.closePanel({json.dumps(panel_id)})')
            wait(lambda: any(value['id'] == target['id'] for value in targets()),
                 lambda value: not value, f'{panel_id} document closed')
            results.append(dict(panel=panel_id, route=route, tab_id=opened['tab_id'], mounted=True,
                                reused=True, docked=True, detached=True, closed=True, settings_save_reset=settings_saved,
                                file_crud=file_crud, minimize_restore=restored))
            opened = None
            child.socket.close()
            child = None
        return dict(archive_session=archive, panels=results)
    except Exception as error:
        diagnostic = child.evaluate('({url:location.href,text:document.body.innerText.slice(0,4000)})') if child else None
        raise AssertionError(dict(completed=results, current=opened, document=diagnostic, error=str(error))) from error
    finally:
        if opened is not None:
            main.evaluate(f'{store}.closePanel({json.dumps(panel_id)})')
        if child is not None:
            child.socket.close()
        main.socket.close()
