"""Exercise every registered packaged Vue panel in the existing native Dock."""
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
from urllib.request import urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def click_outside_retirement(child):
    """Unmount the actual import dropdown during its listener-registration microtasks."""
    child.evaluate("""(async()=>{const p=window.__outsideLifetime={route:location.hash,add:document.addEventListener,
      remove:document.removeEventListener,active:new Set()};
      document.addEventListener=function(type,handler,...args){if(handler===p.handler)p.active.add(type);
        return p.add.call(this,type,handler,...args);};
      document.removeEventListener=function(type,handler,...args){if(handler===p.handler)p.active.delete(type);
        return p.remove.call(this,type,handler,...args);};
      const root=document.querySelector('[data-guidance=scene-import-model]');
      root.querySelector('button').click();await Promise.resolve();await Promise.resolve();
      p.node=root.querySelector('div');p.handler=p.node?.__clickOutsideHandler__;
      if(typeof p.handler!=='function')throw Error('actual import dropdown directive missing');
      await document.querySelector('#app').__vue_app__.config.globalProperties.$router.push(
        '/ProjectSettings'+p.route.slice(p.route.indexOf('?')));
      await Promise.resolve();await Promise.resolve();p.retired=!document.contains(p.node);
      if(!p.retired)throw Error('import dropdown was not retired before timer registration');return true;})()""")
    try:
        child.evaluate('new Promise(resolve=>setTimeout(()=>resolve(true),60))')
        listeners = child.evaluate('[...window.__outsideLifetime.active]')
        assert not listeners, f'Retired import dropdown registered document listeners: {listeners}'
    finally:
        child.evaluate("(()=>{const p=window.__outsideLifetime;document.addEventListener=p.add;"
                       "document.removeEventListener=p.remove;for(const type of ['mousedown','touchstart'])"
                       "p.remove.call(document,type,p.handler,true);location.hash=p.route;})()")
    wait(lambda: child.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'SceneTools remount after dropdown')
    return dict(retired_document_listeners=listeners, normal_remount=True)


def scene_camera_retirement(child):
    """Hold a real camera creation reply until the actual SceneTools route unmounts."""
    button = 'button[title="添加摄像头"]'
    wait(lambda: child.evaluate(f'!!document.querySelector({json.dumps(button)})'), bool, 'camera create button')
    child.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.createCamera==='function');
      const app=Object.values(m).find(v=>typeof v?.openNativePanel==='function');
      if(!api||!app)throw Error('camera creation services missing');
      const p=window.__sceneCameraLifetime={api,app,create:api.scene.createCamera,
        open:app.createCameraView,route:location.hash,ready:false,retiredOpens:0};
      api.scene.createCamera=(...args)=>p.create(...args).then(result=>new Promise(resolve=>{
        p.args=args;p.camera=result.camera;p.release=()=>resolve(result);p.ready=true;}));
      app.createCameraView=(...args)=>{p.retiredOpens++;throw Error('probe blocked retired camera surface');};
      return true;})()""")
    try:
        child.evaluate(f'document.querySelector({json.dumps(button)}).click()')
        wait(lambda: child.evaluate('window.__sceneCameraLifetime.ready'), bool, 'native camera creation reply held')
        child.evaluate("""(async()=>{const p=window.__sceneCameraLifetime;
          await document.querySelector('#app').__vue_app__.config.globalProperties.$router.push(
            '/ProjectSettings'+p.route.slice(p.route.indexOf('?')));
          if(document.querySelector('.scene-tools-panel'))throw Error('SceneTools not retired');
          p.release();return true;})()""")
        child.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        retired_opens = child.evaluate('window.__sceneCameraLifetime.retiredOpens')
        assert retired_opens == 0, f'Retired SceneBar requested {retired_opens} camera surfaces'
    finally:
        child.evaluate("""(async()=>{const p=window.__sceneCameraLifetime;
          p.api.scene.createCamera=p.create;p.app.createCameraView=p.open;p.release?.();
          if(p.camera)await p.api.scene.deleteCamera(p.args[0],p.camera.camera_id,
            {archive_session:p.args[1].archive_session});
          await document.querySelector('#app').__vue_app__.config.globalProperties.$router.push(p.route.slice(1));
          return true;})()""")
    wait(lambda: child.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'SceneTools camera remount')
    return dict(retired_surface_requests=retired_opens, normal_remount=True)


def model_node_retirement(child):
    """Deliver a real node mutation reply after its packaged SceneTools view retires."""
    tag = '[data-testid="model-node-tag"]'
    wait(lambda: child.evaluate(f'!!document.querySelector({json.dumps(tag)})'), bool, 'model node toggle')
    child.evaluate(f'document.querySelector({json.dumps(tag)}).click()')
    button = '.model-node-list button[title="隐藏"],.model-node-list button[title="显示"]'
    wait(lambda: child.evaluate(f'!!document.querySelector({json.dumps(button)})'), bool, 'real model node row')
    child.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.setModelNodeState==='function');
      if(!api)throw Error('model node facade missing');
      const p=window.__modelLifetime={api,write:api.scene.setModelNodeState,route:location.hash,reads:0,ready:false};
      api.scene.setModelNodeState=(...args)=>p.write(...args).then(result=>new Promise(resolve=>{
        p.identity=args[0];p.release=()=>resolve(new Proxy(result,{get(target,key,receiver){
          if(key==='actor_version'||key==='nodes')p.reads++;return Reflect.get(target,key,receiver);}}));
        p.ready=true;}));return true;})()""")
    try:
        wait(lambda: child.evaluate(f'(()=>{{const button=document.querySelector({json.dumps(button)});'
                                    'if(!button)return false;button.click();return true;})()'),
             bool, 'click live model node row')
        wait(lambda: child.evaluate('window.__modelLifetime.ready'), bool, 'real model mutation reply held')
        child.evaluate("location.hash='/ProjectSettings'+window.__modelLifetime.route.slice(window.__modelLifetime.route.indexOf('?'))")
        wait(lambda: child.evaluate("!!document.querySelector('.scene-tools-panel')"), lambda value: not value,
             'model node view retired')
        child.evaluate('window.__modelLifetime.release()')
        child.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        reads = child.evaluate('window.__modelLifetime.reads')
        assert reads == 0, f'Retired ModelNodeHierarchy consumed {reads} mutation reply fields'
    finally:
        child.evaluate("(()=>{const p=window.__modelLifetime;p.api.scene.setModelNodeState=p.write;p.release?.();location.hash=p.route;})()")
    wait(lambda: child.evaluate(f'!!document.querySelector({json.dumps(tag)})'), bool, 'model node normal remount')
    child.evaluate(f'document.querySelector({json.dumps(tag)}).click()')
    initial_title = wait(lambda: child.evaluate(
        f'(()=>{{const button=document.querySelector({json.dumps(button)});'
        'if(!button||button.disabled)return null;const title=button.title;button.click();return title;})()'),
        bool, 'click model node remounted row and capture its initial visibility')
    wait(lambda: child.evaluate(f'document.querySelector({json.dumps(button)})?.title'),
         lambda value: value and value != initial_title, 'live model visibility edit after remount')
    return dict(late_mutation_field_reads=reads, normal_remount=True, live_visibility_edit=True)


def editor_settings_retirement(child):
    """Hold an actual masked settings reply across a real Vue route retirement."""
    child.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const settings=Object.values(m).find(v=>typeof v?.get==='function'&&typeof v?.saveApiKey==='function');
      if(!settings)throw Error('shared AI settings service missing');
      const probe=window.__settingsLifetime={settings,get:settings.get,route:location.hash,reads:0,ready:false};
      settings.get=(...args)=>probe.get(...args).then(result=>new Promise(resolve=>{
        probe.release=()=>resolve(new Proxy(result,{get(target,key,receiver){
          if(key==='configured'||key==='masked_key')probe.reads++;return Reflect.get(target,key,receiver);}}));
        probe.ready=true;}));return true;})()""")
    away = "location.hash='/ProjectSettings'+window.__settingsLifetime.route.slice(window.__settingsLifetime.route.indexOf('?'))"
    try:
        child.evaluate(away)
        wait(lambda: child.evaluate("!!document.querySelector('.esc-panel')"), lambda value: not value,
             'initial settings view retired')
        child.evaluate('location.hash=window.__settingsLifetime.route')
        wait(lambda: child.evaluate('window.__settingsLifetime.ready'), bool, 'real AI settings reply held')
        child.evaluate(away)
        wait(lambda: child.evaluate("!!document.querySelector('.esc-panel')"), lambda value: not value,
             'pending settings view retired')
        child.evaluate('window.__settingsLifetime.release()')
        child.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        reads = child.evaluate('window.__settingsLifetime.reads')
        assert reads == 0, f'Retired EditorSettings consumed {reads} masked settings fields'
    finally:
        child.evaluate("(()=>{const p=window.__settingsLifetime;p.settings.get=p.get;p.release?.();location.hash=p.route;})()")
    wait(lambda: child.evaluate("!!document.querySelector('.esc-panel')"), bool, 'normal settings remount')
    return dict(late_settings_field_reads=reads, normal_remount=True)


def minimize_restore(engine_pid, main, child, targets, service):
    """Minimize only this launch's real HWNDs, then verify both CEF documents survive."""
    user = ctypes.WinDLL('user32', use_last_error=True)
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user.EnumWindows.argtypes = [callback, wintypes.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.IsIconic.argtypes = [wintypes.HWND]
    user.ShowWindowAsync.argtypes = [wintypes.HWND, ctypes.c_int]
    user.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    windows = []
    native_windows = []

    @callback
    def visit(hwnd, _):
        pid = wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == engine_pid:
            title = ctypes.create_unicode_buffer(512)
            user.GetWindowTextW(hwnd, title, len(title))
            visible = bool(user.IsWindowVisible(hwnd))
            native_windows.append(dict(hwnd=hwnd, title=title.value, visible=visible,
                                       minimized=bool(user.IsIconic(hwnd))))
            if visible:
                windows.append(hwnd)
        return True

    def visible_windows():
        windows.clear()
        native_windows.clear()
        assert user.EnumWindows(visit, 0)
        return dict(visible=list(windows), native=list(native_windows))

    # Detach state commits before the hidden SDL window's first present makes
    # it visible. Minimize the actual HWNDs only after both are shown.
    snapshot = wait(visible_windows, lambda values: len(values['visible']) >= 2,
                    'main and detached native HWNDs visible after first present')
    windows = snapshot['visible']
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
            settings_lifetime = editor_settings_retirement(child) if panel_id == 'EditorSettings' else None
            camera_lifetime = scene_camera_retirement(child) if panel_id == 'SceneTools' else None
            model_lifetime = model_node_retirement(child) if panel_id == 'SceneTools' else None
            outside_lifetime = click_outside_retirement(child) if panel_id == 'SceneTools' else None
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
            # ImGui owns placement, so the panel keeps the same CEF document through open,
            # focus and close; there is no native detach transition left to exercise.
            assert child.evaluate(f'!!document.querySelector({json.dumps(selectors[panel_id])})')
            restored = minimize_restore(engine_pid, main, child, targets, service) if panel_id == 'FileManager' else None
            exceptions = [event['params'] for event in child.events if event.get('method') == 'Runtime.exceptionThrown']
            assert not exceptions, (panel_id, exceptions)
            main.evaluate(f'{store}.closePanel({json.dumps(panel_id)})')
            wait(lambda: any(value['id'] == target['id'] for value in targets()),
                 lambda value: not value, f'{panel_id} document closed')
            results.append(dict(panel=panel_id, route=route, tab_id=opened['tab_id'], mounted=True,
                                reused=True, closed=True, settings_save_reset=settings_saved,
                                file_crud=file_crud, minimize_restore=restored))
            results[-1]['editor_settings_lifetime'] = settings_lifetime
            results[-1]['model_node_lifetime'] = model_lifetime
            results[-1]['click_outside_lifetime'] = outside_lifetime
            results[-1]['scene_camera_lifetime'] = camera_lifetime
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
