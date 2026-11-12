"""Exercise startup calibration and cross-document Vue edits in packaged CEF."""
import json
import traceback
from urllib.request import urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def run(port, root):
    del root  # The runner owns the disposable project and isolated CEF cache.

    def targets():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
            return [p for p in json.load(response) if p['type'] == 'page'
                    and '/CabbageEditor/Frontend/' in p['url']]

    mains = [p for p in targets() if 'uiOwner=' not in p['url'] and 'standalone=1' not in p['url']]
    assert len(mains) == 1, mains
    main = Frontend(port, target_id=mains[0]['id'])
    clients = [main]
    calibration_clients = []
    opened = []
    settings_tab = None
    service = """(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      return Object.values(m).find(v=>typeof v?.openNativePanel==='function');})()"""
    store = "document.querySelector('#app').__vue_app__.config.globalProperties.$pinia._s.get('dock')"
    key = 'corona.viewportUiCalibration.main.scene.default'
    seed = dict(pe=21, angle=.25, offset=12, parallaxScale=18)
    guard = """(()=>{const p=Storage.prototype;
      const original={get:p.getItem,set:p.setItem};
      const guarded=key=>key.startsWith('corona.viewportUiCalibration.')||key==='corona.cabbageAssistantContext.v2';
      window.__calibrationStorage={blockedReads:0,blockedWrites:0,
        read:key=>original.get.call(localStorage,key),
        restore:()=>{p.getItem=original.get;p.setItem=original.set;}};
      p.getItem=function(key){if(guarded(key)){window.__calibrationStorage.blockedReads++;throw Error('test storage unavailable');}
        return original.get.call(this,key);};
      p.setItem=function(key,value){if(guarded(key)){window.__calibrationStorage.blockedWrites++;throw Error('test storage unavailable');}
        return original.set.call(this,key,value);};return true;})()"""

    def values(client):
        return client.evaluate("[...document.querySelectorAll('.lfd-calib input')].map(input=>Number(input.value))")

    def edit(client, index, value):
        client.evaluate(f"(()=>{{const input=document.querySelectorAll('.lfd-calib input')[{index}];"
                        f"input.value={json.dumps(str(value))};input.dispatchEvent(new Event('input',{{bubbles:true}}));"
                        "input.dispatchEvent(new FocusEvent('blur'));return true;})()")

    def open_panel(menu=False):
        before = {p['id'] for p in targets()}
        if menu:
            main.evaluate(f"{store}.openPanel('LightFieldCalibration')")
        else:
            result = main.evaluate(f"{service}.then(s=>s.openNativePanel({{panelId:'LightFieldCalibration',"
                                   "routePath:'/LightFieldCalibration',width:600,height:600}))")
            assert result.get('tab_id'), result
        target = wait(lambda: next((p for p in targets() if (menu or p['id'] not in before)
                                   and '#/LightFieldCalibration?' in p['url']), None), bool, 'new calibration document')
        client = Frontend(port, target_id=target['id'])
        client.panel_id = 'LightFieldCalibration'
        client.target_id = target['id']
        clients.append(client)
        calibration_clients.append(client)
        wait(lambda: values(client), lambda v: len(v) == 4, 'calibration inputs mounted')
        opened.append((client.panel_id, client.target_id))
        client.evaluate(guard)
        return client

    try:
        main.evaluate("location.hash='/';true")
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda v: v and v.get('enabled'), 'initial main owner ready')
        main.evaluate('localStorage.setItem(' + json.dumps(key) + ',' + json.dumps(json.dumps(seed)) + ');true')
        retired = main.evaluate(f'{service}.then(s=>s.setEditorUiEnabled(false))')
        assert retired.get('enabled') is False, retired
        main.call('Page.reload', {'ignoreCache': True})

        def owner_ready():
            try:
                return main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())')
            except Exception:
                return None  # Reload temporarily destroys the previous JS execution context.

        wait(owner_ready, lambda v: v and v.get('enabled'), 'reloaded main owner ready', seconds=60)
        main.evaluate(guard)
        # Calibration is a stereo UI panel; use the production settings action
        # so a still-initializing flat viewport cannot legitimately close it.
        result = main.evaluate(f"{service}.then(s=>s.openNativePanel({{panelId:'EditorSettings',"
                               "routePath:'/SetUp',width:600,height:800}))")
        assert result.get('tab_id'), result
        settings_window = None
        target = wait(lambda: next((p for p in targets() if '#/SetUp?' in p['url']), None), bool, 'settings document')
        settings_window = target['id']
        settings = Frontend(port, target_id=target['id'])
        clients.append(settings)
        wait(lambda: settings.evaluate("!!document.querySelector('[data-guidance=settings-viewport-ui] button:not(:disabled)')"),
             bool, 'viewport mode action ready')
        settings.evaluate("document.querySelectorAll('[data-guidance=settings-viewport-ui] button')[1].click();true")
        wait(lambda: settings.evaluate("document.querySelectorAll('[data-guidance=settings-viewport-ui] button')[1].classList.contains('active')"),
             bool, 'stereo UI mode active')
        first = open_panel(menu=True)
        wait(lambda: values(first), lambda v: v == list(seed.values()), 'persisted startup calibration hydration')
        second = open_panel()
        wait(lambda: values(second), lambda v: v == list(seed.values()), 'second Dock initial owner snapshot')
        edit(first, 0, 23)
        wait(lambda: values(second), lambda v: v[0] == 23, 'first Dock edit reaches second Dock')
        edit(second, 2, 15)
        expected = [23, .25, 15, 18]
        wait(lambda: values(first), lambda v: v == expected, 'second Dock edit reaches first Dock')
        third = open_panel()
        wait(lambda: values(third), lambda v: v == expected, 'late Dock hydrates live memory rather than storage')
        persisted = main.evaluate('window.__calibrationStorage.read(' + json.dumps(key) + ')')
        assert json.loads(persisted) == seed, persisted
        writes = main.evaluate('window.__calibrationStorage.blockedWrites')
        assert writes >= 2, writes
        peers = [client.evaluate('({reads:window.__calibrationStorage.blockedReads,writes:window.__calibrationStorage.blockedWrites})')
                 for client in calibration_clients]
        assert all(p == dict(reads=0, writes=0) for p in peers), peers
        assert all(not client.evaluate("!!document.querySelector('.lfd-calib [role=alert]')") for client in calibration_clients)
        # Route-unmount the actual MainPage while low-level detached replicas
        # remain alive. The registered menu panel follows NativeDockLayout's
        # existing lifecycle; the two replicas exercise the surviving documents.
        identities = [client.target_id for client in (second, third)]
        marker = main.evaluate('window.__calibrationRemount=crypto.randomUUID()')
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('[data-viewport-pick-surface]')"),
             bool, 'real MainPage route unmounted')
        main.evaluate("location.hash='/';true")
        wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'real MainPage route remounted')
        assert main.evaluate('window.__calibrationRemount') == marker, 'route remount replaced the CEF document'
        fourth = open_panel()
        wait(lambda: values(fourth), lambda v: v == expected, 'remounted main retains unsaved live calibration')
        edit(second, 0, 24)
        remounted = [24, .25, 15, 18]
        for client in (third, fourth):
            wait(lambda: values(client), lambda v: v == remounted, 'surviving Dock accepts remounted owner updates')
        assert [client.target_id for client in (second, third)] == identities
        return dict(mainReloadStartupSeed=seed, liveValues=expected, documents=len(clients),
                    bidirectionalVueEdits=True, lateDockHydration=True,
                    rejectedOwnerStorageWrites=writes, peerStorageAccess=peers,
                    mainViewRemount=dict(liveValues=remounted, survivingPanels=identities,
                                         documentPreserved=True, newDockHydration=True),
                    scope='isolated CEF cache; startup page reload before peers; live MainPage route remount with surviving peers')
    except Exception:
        # Keep the actual assertion visible if native retirement also makes
        # cleanup fail; a finally exception otherwise replaces its traceback.
        traceback.print_exc()
        raise
    finally:
        for client in clients:
            try:
                client.evaluate('window.__calibrationStorage?.restore();true')
            except Exception:
                pass
        try:
            main.evaluate(f"{store}.closePanel('LightFieldCalibration')")
            # Store intent starts an asynchronous NativeDock reconciliation.
            # Wait for its real retirement before another close takes an
            # activity snapshot; a departing peer correctly invalidates one.
            if opened:
                wait(lambda: {p['id'] for p in targets()},
                     lambda ids: opened[0][1] not in ids, 'menu calibration retired')
            for panel, window in opened[1:]:
                main.evaluate(f'{service}.then(s=>s.closePanelTab({json.dumps(panel)}))')
                wait(lambda: {p['id'] for p in targets()},
                     lambda ids: window not in ids, 'calibration replica retired')
            if settings_window is not None:
                main.evaluate(f'{service}.then(s=>s.closePanelTab("EditorSettings"))')
                wait(lambda: {p['id'] for p in targets()},
                     lambda ids: settings_window not in ids, 'settings document retired')
        finally:
            for client in clients:
                client.socket.close()
