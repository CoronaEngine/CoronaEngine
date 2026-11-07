"""Exercise every registered packaged Vue panel in the existing native Dock."""
import json
from pathlib import Path
import re
from urllib.request import urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def run(port, root):
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
            exceptions = [event['params'] for event in child.events if event.get('method') == 'Runtime.exceptionThrown']
            assert not exceptions, (panel_id, exceptions)
            main.evaluate(f'{store}.closePanel({json.dumps(panel_id)})')
            wait(lambda: any(value['id'] == target['id'] for value in targets()),
                 lambda value: not value, f'{panel_id} document closed')
            results.append(dict(panel=panel_id, route=route, tab_id=opened['tab_id'], mounted=True,
                                reused=True, docked=True, detached=True, closed=True))
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
