"""Exercise deployed MainPage environment controls through real CEF panels."""
import json
from pathlib import Path
import sys
import time
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from vision_frontend_probe import Frontend


def wait(read, predicate, label, seconds=30):
    end = time.monotonic() + seconds
    last = None
    while time.monotonic() < end:
        last = read()
        if predicate(last):
            return last
        time.sleep(.1)
    raise AssertionError(f"{label}: {last!r}")


def run(port, root):
    def targets():
        with urlopen(f"http://127.0.0.1:{port}/json/list", timeout=3) as response:
            return [p for p in json.load(response) if p["type"] == "page" and "/CabbageEditor/Frontend/" in p["url"]]

    main = Frontend(port, target_id=targets()[0]["id"])
    clients, opened = [main], []
    session = main.evaluate("corona.scene.readActive()")['archive_session']
    def environment():
        return main.evaluate("corona.scene.getEnvironment('scene.ini'," + json.dumps({'archive_session': session}) + ")")

    service = """(async()=>{const module=await import(document.querySelector('script[type=module]').src);
      return Object.values(module).find(value=>typeof value?.openNativePanel==='function');})()"""
    monitor = """(()=>{window.__oldEnvironmentRequests=[];const invoke=corona.editor.invoke;
      corona.editor.invoke=(api,args)=>{if(/^SceneTools\\.(?:get_environment|set_environment|sun_direction|floor_grid|get_physics_params|set_physics_params)$/.test(api)){
        window.__oldEnvironmentRequests.push(api);throw Error('Retired environment transport');}
        return invoke(api,args);};return true;})()"""
    def open_panel(panel_id, route, selector):
        result = main.evaluate(f"{service}.then(service=>service.openNativePanel({{detached:true,panelId:{json.dumps(panel_id)},routePath:{json.dumps(route)},width:600,height:800}}))")
        assert result.get('tab_id'), result
        opened.append(result['tab_id'])
        target = wait(lambda: next((p for p in targets() if 'standalone=1' in p['url'] and f'#/{route.lstrip("/")}?' in p['url']), None), bool, panel_id)
        client = Frontend(port, target_id=target['id'])
        clients.append(client)
        wait(lambda: client.evaluate(f"!!document.querySelector({json.dumps(selector)})"), bool, panel_id + ' ready')
        client.evaluate(monitor)
        return client

    try:
        main.evaluate("localStorage.setItem('corona.activeProjectPath'," + json.dumps(str(root)) + "); location.hash='/'")
        wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'MainPage mount')
        wait(lambda: main.evaluate(f"{service}.then(service=>service.readEditorUiPolicy())"), lambda policy: policy and policy.get('enabled'), 'Web owner ready')
        main.evaluate(monitor)
        original = environment()
        controls = open_panel('SceneControls', '/SceneControls', '.scene-quick-light-toggle input:not(:disabled)')
        settings = open_panel('EditorSettings', '/SetUp', '[data-guidance=settings-grid] input:not(:disabled)')

        controls.evaluate("document.querySelector('.scene-quick-light-toggle input').click()")
        wait(environment, lambda value: value['sun']['enabled'] is not original['sun']['enabled'], 'panel sun patch')
        settings.evaluate("document.querySelector('[data-guidance=settings-grid] input').click()")
        wait(environment, lambda value: value['grid']['enabled'] is not original['grid']['enabled'], 'settings grid patch')
        if not environment()['sun']['enabled']:
            wait(lambda: controls.evaluate("!document.querySelector('.scene-quick-light-toggle input').disabled"), bool, 'sun control idle')
            controls.evaluate("document.querySelector('.scene-quick-light-toggle input').click()")
            wait(environment, lambda value: value['sun']['enabled'], 'enable direction controls')
        wait(lambda: controls.evaluate("!document.querySelector('.scene-quick-direction input').disabled"), bool, 'direction control ready')
        controls.evaluate("(()=>{const input=document.querySelector('.scene-quick-direction input');input.value='3';input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('change',{bubbles:true}));})()")
        wait(environment, lambda value: value['sun']['direction'][0] == 3, 'direction patch')
        controls.evaluate("document.querySelector('.physics-fields').closest('details').open=true")
        wait(lambda: controls.evaluate("Number(document.querySelectorAll('.physics-fields input')[3].value)"),
             lambda value: value == original['physics']['floor_y'], 'scoped physics read')
        controls.evaluate("(()=>{const form=document.querySelector('.physics-fields'),input=form.querySelectorAll('input')[3];input.value='6';input.dispatchEvent(new Event('input',{bubbles:true}));form.dispatchEvent(new Event('submit',{bubbles:true,cancelable:true}));})()")
        changed = wait(environment, lambda value: value['physics']['floor_y'] == 6, 'physics form patch')
        assert changed['physics']['gravity'] == original['physics']['gravity'], changed
        assert changed['sun']['direction'][0] == 3 and changed['grid']['enabled'] is not original['grid']['enabled'], changed
        saved = main.api('MainView.scene_save', ['scene.ini', {'archive_session': session}])
        assert saved['success'] and saved['data']['ok'], saved
        # Reopening retires detached documents; inspect their monitors while alive.
        for client in clients:
            assert client.evaluate('window.__oldEnvironmentRequests') == []
        old_scope = session
        reopened = main.api('ProjectLauncher.open_project', [str(root)])
        assert reopened['success'] and reopened['data']['ok'], reopened
        session = main.evaluate('corona.scene.readActive()')['archive_session']
        assert session != old_scope
        assert environment()['physics']['floor_y'] == 6
        rejected = main.evaluate("corona.scene.setEnvironment('scene.ini'," + json.dumps({
            'archive_session': old_scope, 'grid': {'enabled': original['grid']['enabled']},
        }) + ").catch(error=>error.code)")
        assert rejected == 'PROJECT_CONTEXT_CHANGED', rejected
        assert main.evaluate('window.__oldEnvironmentRequests') == []
        return {'documents': len(clients), 'uiWrites': ['sun.enabled', 'sun.direction', 'grid.enabled', 'physics.floor_y'],
                'samePhysicsGravity': True, 'saveReopenFloorY': environment()['physics']['floor_y'],
                'oldScopeRejected': rejected, 'retiredEnvironmentRequests': 0}
    finally:
        for tab_id in opened:
            main.evaluate(f'corona.window.closeTab({tab_id})')
        for client in clients:
            client.socket.close()
