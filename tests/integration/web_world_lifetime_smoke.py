"""Retire real MainPage work while native project opens finish out of selection order."""
import json
from urllib.request import urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def run(port, root):
    with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
        targets = [target for target in json.load(response) if target['type'] == 'page'
                   and '/CabbageEditor/Frontend/' in target['url'] and 'uiOwner=' not in target['url']]
    assert len(targets) == 1, targets
    main = Frontend(port, target_id=targets[0]['id'])
    worlds = []
    for name in ('intermediate', 'latest'):
        world = root.parent / ('lifetime-' + name)
        world.mkdir()
        (world / 'scene.ini').write_text(
            '[format]\ntype=corona_scene_folder\nversion=1\n[scene]\nname=' + name
            + '\n[world]\ntype=creative\n[actors]\n[scripts]\npath=\n[terrain]\ntype=\npath=\n'
            '[camera]\ncount=1\ncamera0.id=' + name + '-camera\ncamera0.name=' + name
            + '\ncamera0.render_backend=vision\ncamera0.width=320\ncamera0.height=240\n', encoding='utf-8')
        (world / 'assets.manifest.json').write_text(
            '{"format":"corona_scene_assets","version":1,"bundles":[]}', encoding='utf-8')
        worlds.append(world)
    try:
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('[data-viewport-surface]')"),
             bool, 'main viewport retired before arming initialization')
        initial = main.evaluate('corona.scene.readActive()')
        main.evaluate("""(async()=>{
          const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.scene?.readScene==='function');
          const launcher=Object.values(m).find(v=>typeof v?.openProject==='function'&&!v.createWorldProject);
          if(!launcher)throw Error('Production project launcher export missing');
          const probe=window.__worldLifetime={launcher,pendingRead:[],pendingOpen:[],commands:[],holdRead:true,holdOpen:true};
          const original={read:api.scene.readScene,open:api.project.openProject,
            properties:api.scene.updateCamera,render:api.scene.setCameraRenderSettings,viewport:api.viewport.configure};
          api.scene.readScene=(...args)=>{
            const hold=probe.holdRead&&document.querySelector('[data-viewport-surface]');
            if(hold)probe.holdRead=false;
            const result=original.read(...args);
            return hold?Promise.resolve(result).then(value=>new Promise(resolve=>probe.pendingRead.push(()=>resolve(value)))):result;
          };
          api.project.openProject=(...args)=>{
            const hold=probe.holdOpen;probe.holdOpen=false;
            const result=original.open(...args);
            return hold?Promise.resolve(result).then(value=>new Promise(resolve=>probe.pendingOpen.push(()=>resolve(value)))):result;
          };
          for(const [kind,object,key] of [['properties',api.scene,'updateCamera'],
            ['render',api.scene,'setCameraRenderSettings'],['viewport',api.viewport,'configure']]){
            object[key]=(...args)=>{probe.commands.push({kind,args});return original[kind](...args);};
          }
          probe.release=key=>{for(const resume of probe[key].splice(0))resume();};
          probe.restore=()=>{probe.release('pendingRead');probe.release('pendingOpen');
            api.scene.readScene=original.read;api.project.openProject=original.open;
            api.scene.updateCamera=original.properties;api.scene.setCameraRenderSettings=original.render;
            api.viewport.configure=original.viewport;};
          return true;
        })()""")
        main.evaluate("location.hash='/';true")
        wait(lambda: main.evaluate('window.__worldLifetime.pendingRead.length'), lambda n: n == 1,
             'old MainPage real camera snapshot waiting for its consumer')
        main.evaluate('window.__worldLifetime.first=window.__worldLifetime.launcher.openProject('
                      + json.dumps(str(worlds[0])) + ');true')
        wait(lambda: main.evaluate('window.__worldLifetime.pendingOpen.length'), lambda n: n == 1,
             'intermediate native open completed before its launcher continuation')
        intermediate = main.evaluate('corona.scene.readActive()')
        assert intermediate['archive_session'] != initial['archive_session'], intermediate
        main.evaluate('window.__worldLifetime.latest=window.__worldLifetime.launcher.openProject('
                      + json.dumps(str(worlds[1])) + ');window.__worldLifetime.release("pendingOpen");true')
        first = main.evaluate('window.__worldLifetime.first')
        assert first == {'ok': False, 'status': 'superseded'}, first
        latest = main.evaluate('window.__worldLifetime.latest')
        assert latest.get('ok'), latest
        current = main.evaluate('corona.scene.readActive()')
        assert current['archive_session'] not in (initial['archive_session'], intermediate['archive_session']), current
        wait(lambda: main.call('Runtime.evaluate', {
            'expression': "Boolean(document.querySelector('[data-viewport-surface]')&&getEventListeners(document).keydown?.some(e=>e.listener.toString().includes('new-tab-name')))",
            'includeCommandLineAPI': True, 'returnByValue': True,
        })['result'].get('value'), bool, 'latest MainPage input fully initialized')
        main.evaluate('window.__worldLifetime.commands=[];window.__worldLifetime.release("pendingRead");true')
        # Read through the native boundary after the old continuation has run.
        assert main.evaluate('corona.scene.readActive()')['archive_session'] == current['archive_session']
        commands = main.evaluate('window.__worldLifetime.commands')
        assert all(call['args'][2]['archive_session'] == current['archive_session'] for call in commands), commands
        main.evaluate("document.dispatchEvent(new KeyboardEvent('keydown',{bubbles:true,code:'KeyW',key:'w'}));true")
        try:
            movement = wait(lambda: main.evaluate('window.__worldLifetime.commands.filter(c=>Array.isArray(c.args[2]?.position))'),
                            bool, 'latest MainPage actual input writes its current camera')
        finally:
            main.evaluate("document.dispatchEvent(new KeyboardEvent('keyup',{bubbles:true,code:'KeyW',key:'w'}));true")
        assert all(call['args'][1] == 'latest-camera'
                   and call['args'][2]['archive_session'] == current['archive_session'] for call in movement), movement
        commands = main.evaluate('window.__worldLifetime.commands')
        assert all(call['args'][2]['archive_session'] == current['archive_session'] for call in commands), commands
        listener_count = main.call('Runtime.evaluate', {
            'expression': "(getEventListeners(document).keydown||[]).filter(e=>e.listener.toString().includes('new-tab-name')).length",
            'includeCommandLineAPI': True, 'returnByValue': True,
        })['result']['value']
        assert listener_count == 1, 'world switches retained duplicate MainPage input listeners'
        return dict(initialArchive=initial['archive_session'], intermediateArchive=intermediate['archive_session'],
                    finalArchive=current['archive_session'], intermediateSuperseded=True, oldSnapshotRetired=True,
                    currentCameraInput=True, retiredNativeCommands=0, mainInputListeners=listener_count)
    finally:
        try:
            main.evaluate('window.__worldLifetime?.restore();true')
        finally:
            main.socket.close()
