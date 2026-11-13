"""Exercise deployed MainPage environment controls through real CEF panels."""
import json
from pathlib import Path
import sys
import time
from urllib.request import urlopen, url2pathname
from urllib.parse import urlsplit

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


def main_render_retirement(main, controls):
    """Deliver a real DOM-initiated native mode reply after MainPage unmounts."""
    main.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.setCameraRenderSettings==='function');
      if(!api)throw Error('main render facade missing');
      const p=window.__mainRenderLifetime={api,write:api.scene.setCameraRenderSettings,
        route:location.hash,ready:false,reads:0,phase:'before',versions:[],post:BroadcastChannel.prototype.postMessage};
      BroadcastChannel.prototype.postMessage=function(message){
        if(message.topic==='creative-controls-state')p.versions.push({phase:p.phase,revision:message.payload.revision});
        return p.post.call(this,message);};
      api.scene.setCameraRenderSettings=(...args)=>p.write(...args).then(result=>{
        if(args[2]?.render_backend!=='native')return result;
        return new Promise(resolve=>{p.release=()=>resolve(new Proxy(result,{get(target,key,receiver){
          if(['render_backend','vision','pending'].includes(key))p.reads++;
          return Reflect.get(target,key,receiver);}}));p.ready=true;});});return true;})()""")
    try:
        wait(lambda: controls.evaluate("!document.querySelector('.scene-quick-render-trigger').disabled"
                                      "&&!document.querySelector('[data-guidance=render-settings]').inert"),
             bool, 'main render controls ready')
        controls.evaluate("document.querySelector('.scene-quick-render-trigger').click()")
        wait(lambda: controls.evaluate("!!document.querySelector('.scene-quick-render-dropdown [role=menuitemradio]')"),
             bool, 'main mode menu')
        controls.evaluate("[...document.querySelectorAll('.scene-quick-render-dropdown [role=menuitemradio]')]"
                          ".find(button=>button.textContent.trim()==='Native').click()")
        wait(lambda: main.evaluate('window.__mainRenderLifetime.ready'), bool, 'real main mode reply held')
        main.evaluate("""(async()=>{const p=window.__mainRenderLifetime;
          await document.querySelector('#app').__vue_app__.config.globalProperties.$router.push('/ProjectSettings');
          if(document.querySelector('[data-viewport-pick-surface]'))throw Error('MainPage was not retired');
          p.release();return true;})()""")
        main.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        reads = main.evaluate('window.__mainRenderLifetime.reads')
        assert reads == 0, f'Retired MainPage consumed {reads} render reply fields'
    finally:
        main.evaluate("""(async()=>{const p=window.__mainRenderLifetime;
          p.api.scene.setCameraRenderSettings=p.write;p.release?.();
          p.phase='after';
          try{await document.querySelector('#app').__vue_app__.config.globalProperties.$router.push(p.route.slice(1)||'/');
            await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
          }finally{BroadcastChannel.prototype.postMessage=p.post;}
          return true;})()""")
    wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'MainPage normal remount')
    wait(lambda: controls.evaluate("!document.querySelector('.scene-quick-render-trigger').disabled"
                                  "&&!document.querySelector('[data-guidance=render-settings]').inert"),
         bool, 'render controls reconnected')
    # The creative panel provider starts in setup; viewport controls register
    # only after native camera restoration and render initialization complete.
    entry = main.evaluate("document.querySelector('script[type=module]').src")
    chunks = list(Path(url2pathname(urlsplit(entry).path)).parent.glob('viewportControlsService-*.js'))
    assert len(chunks) == 1, chunks
    main.evaluate('window.__viewportServiceUrl=' + json.dumps(chunks[0].resolve().as_uri()))
    main.evaluate("""(async()=>{const m=await import(window.__viewportServiceUrl);
      const service=Object.values(m).find(v=>typeof v?.request==='function'&&typeof v?.provide==='function'
        &&typeof v?.publish==='function');if(!service)throw Error('viewport controls service missing');
      const state=await service.request({action:'getState'});if(!state.available||!state.cameraId)
        throw Error('viewport controls not restored');return true;})()""")
    versions = main.evaluate('window.__mainRenderLifetime.versions')
    before = [value['revision'] for value in versions if value['phase'] == 'before']
    after = [value['revision'] for value in versions if value['phase'] == 'after']
    assert before and after and min(after) > max(before), versions
    return dict(retired_render_reply_reads=reads, normal_remount=True, provider_versions=versions)


def main_selection_retirement(main, open_panel, root, session):
    """A real SceneBar click must not target a gizmo or reopen Object after unmount."""
    # The viewport and controls appear before MainPage's final initialization
    # snapshot. Wait for initialization, then gate every index reply before
    # creating the Actor so a background refresh cannot cache it before click.
    wait(lambda: main.call('Runtime.evaluate', {
        'expression': "Boolean(getEventListeners(window)['cabbage-run-failed']?.length)",
        'includeCommandLineAPI': True, 'returnByValue': True,
    })['result'].get('value'), bool, 'MainPage final mounted lifecycle ready')
    actor = None
    try:
        tools = open_panel('SceneTools', '/SceneBar', '[data-guidance=scene-import-model]')
        main.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.scene?.readScene==='function');
          const dock=document.querySelector('#app').__vue_app__.config.globalProperties.$pinia._s.get('dock');
          if(!api?.viewport?.setGizmoTarget||!dock)throw Error('selection boundaries missing');
          await dock.closePanel('Object');
          const p=window.__selectionLifetime={api,dock,read:api.scene.readScene,target:api.viewport.setGizmoTarget,
            open:dock.openPanel,route:location.hash,ready:false,requests:0,releases:[],retired:false,effects:[]};
          p.release=()=>{for(const release of p.releases.splice(0))release();};
          api.scene.readScene=(...args)=>{if(args[0]?.actors!=='index')return p.read(...args);p.requests++;
            return p.read(...args).then(result=>new Promise(resolve=>{
              if(p.retired){resolve(result);return;}
              p.releases.push(()=>resolve(result));p.ready=true;}));};
          api.viewport.setGizmoTarget=(...args)=>{if(p.retired)p.effects.push({kind:'gizmo',args});return p.target(...args);};
          dock.openPanel=(...args)=>{if(p.retired&&args[0]==='Object')p.effects.push({kind:'open',args});return p.open(...args);};
          return true;})()""")
        try:
            actor = main.evaluate("corona.scene.createActor('scene.ini'," + json.dumps(str(root / 'triangle.ply'))
                                  + ',' + json.dumps({'archive_session': session, 'actor_type': 'model',
                                                     'actor_name': 'SelectionLifetime'}) + ')')
            wait(lambda: tools.evaluate("!!document.querySelector('[data-actor-name=SelectionLifetime]')"),
                 bool, 'new selection Actor row')
            requests = main.evaluate('window.__selectionLifetime.requests')
            tools.evaluate("document.querySelector('[data-actor-name=SelectionLifetime]').click()")
            wait(lambda: main.evaluate('window.__selectionLifetime.requests'),
                 lambda value: value > requests, 'selection starts a fresh Actor index request')
            wait(lambda: main.evaluate('window.__selectionLifetime.ready'), bool, 'real selection Actor index reply held')
            main.evaluate("""(async()=>{const p=window.__selectionLifetime;
              await document.querySelector('#app').__vue_app__.config.globalProperties.$router.push('/ProjectSettings');
              if(document.querySelector('[data-viewport-pick-surface]'))throw Error('MainPage still mounted');
              p.retired=true;p.release();return true;})()""")
            main.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
            effects = main.evaluate('window.__selectionLifetime.effects')
            assert not effects, f'Retired main selection continued native/Dock effects: {effects}'
        finally:
            main.evaluate("""(async()=>{const p=window.__selectionLifetime;p.retired=true;p.api.scene.readScene=p.read;
              p.api.viewport.setGizmoTarget=p.target;p.dock.openPanel=p.open;p.release?.();
              await document.querySelector('#app').__vue_app__.config.globalProperties.$router.push(p.route.slice(1)||'/');
              return true;})()""")
        wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'MainPage selection remount')
        wait(lambda: main.call('Runtime.evaluate', {
            'expression': "Boolean(getEventListeners(window)['cabbage-run-failed']?.length)",
            'includeCommandLineAPI': True, 'returnByValue': True,
        })['result'].get('value'), bool, 'MainPage selection remount initialized')
        return dict(retired_effects=effects, normal_remount=True), tools
    finally:
        if actor:
            main.evaluate("corona.scene.deleteActor('scene.ini'," + json.dumps(actor['actor_guid'])
                          + ',' + json.dumps({'archive_session': session}) + ')')


def main_transform_baseline_race(main, tools, session):
    """A held real property sample must not replay an older transform baseline."""
    scope = {'archive_session': session}
    original = main.evaluate("corona.scene.readActorProperties('scene.ini','Probe'," + json.dumps(scope) + ')')
    entry = main.evaluate("document.querySelector('script[type=module]').src")
    chunks = list(Path(url2pathname(urlsplit(entry).path)).parent.glob('cabbageAssistantContextService-*.js'))
    assert len(chunks) == 1, chunks
    main.evaluate('window.__transformContextUrl=' + json.dumps(chunks[0].resolve().as_uri()))
    main.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.readActorProperties==='function');
      const services=await import(window.__transformContextUrl);
      const context=Object.values(services).find(v=>typeof v?.recordEvent==='function'&&typeof v?.notifySceneSaved==='function');
      if(!api||!context)throw Error('transform boundaries missing');
      const p=window.__transformBaseline={api,context,read:api.scene.readActorProperties,
        transforms:api.scene.readTransforms,record:context.recordEvent,held:false,ready:false,samples:[],events:[]};
      api.scene.readActorProperties=(...args)=>{if(p.held||args[1]!=='Probe')return p.read(...args);p.held=true;
        return p.read(...args).then(result=>new Promise(resolve=>{p.release=()=>resolve(result);p.ready=true;}));};
      api.scene.readTransforms=(...args)=>p.transforms(...args).then(result=>{p.samples.push(...result.actors);return result;});
      context.recordEvent=(event,...args)=>{if(event.type==='transform_position'&&event.details?.source==='viewport'
        &&event.details?.actorName==='Probe')p.events.push(event);return p.record(event,...args);};return true;})()""")
    try:
        tools.evaluate("document.querySelector('[data-actor-name=Probe]').click()")
        wait(lambda: main.evaluate('window.__transformBaseline.ready'), bool, 'real property baseline held')
        wait(lambda: main.evaluate('window.__transformBaseline.samples'),
             lambda values: any(value['actor_guid'] == original['actor_guid'] for value in values), 'initial live transform echo')
        main.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        patch = {**scope, 'position': [original['geometry']['position'][0] + 4, *original['geometry']['position'][1:]]}
        def write():
            return main.evaluate("corona.scene.setActorTransform('scene.ini','Probe'," + json.dumps(patch) + ')')
        write()
        wait(lambda: main.evaluate('window.__transformBaseline.events.length'), lambda count: count >= 1, 'live transform recorded')
        before = main.evaluate('window.__transformBaseline.events.length')
        main.evaluate('window.__transformBaseline.release()')
        main.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        repeated = write()
        wait(lambda: main.evaluate('window.__transformBaseline.samples'),
             lambda values: any(value['actor_guid'] == original['actor_guid']
                               and value['actor_version'] == repeated['actor']['actor_version'] for value in values),
             'same-position native version delivered')
        main.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        after = main.evaluate('window.__transformBaseline.events.length')
        assert after == before, f'Old property sample manufactured a second transform event: {before} -> {after}'
        return dict(live_transform_events=before, after_old_baseline_and_same_value=after)
    finally:
        main.evaluate("(()=>{const p=window.__transformBaseline;p.api.scene.readActorProperties=p.read;"
                      "p.api.scene.readTransforms=p.transforms;p.context.recordEvent=p.record;p.release?.();return true;})()")
        main.evaluate("corona.scene.setActorTransform('scene.ini','Probe',"
                      + json.dumps({**scope, **original['geometry']}) + ')')


def main_environment_projection_race(main, controls, settings, environment):
    """A mode refresh must not replay an environment sample older than a Dock edit."""
    original = environment()['grid']['enabled']
    main.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.readEnvironment==='function');
      const p=window.__environmentRace={api,read:api.scene.readEnvironment,held:false,ready:false};
      api.scene.readEnvironment=(...args)=>{if(p.held)return p.read(...args);p.held=true;p.args=args;
        return p.read(...args).then(result=>new Promise(resolve=>{p.release=()=>resolve(result);p.ready=true;}));};
      return true;})()""")

    def select_mode(label):
        controls.evaluate("document.querySelector('.scene-quick-render-trigger').click()")
        wait(lambda: controls.evaluate("!!document.querySelector('.scene-quick-render-dropdown [role=menuitemradio]')"),
             bool, 'render menu for environment race')
        controls.evaluate("[...document.querySelectorAll('.scene-quick-render-dropdown [role=menuitemradio]')]"
                          ".find(button=>button.textContent.trim()===" + json.dumps(label) + ").click()")

    state = """(async()=>{const m=await import(window.__viewportServiceUrl);
      const service=Object.values(m).find(v=>typeof v?.request==='function'&&typeof v?.provide==='function');
      return service.request({action:'getState'});})()"""
    try:
        select_mode('Vision ReSTIR')
        wait(lambda: main.evaluate('window.__environmentRace.ready'), bool, 'real old environment sample held')
        wait(lambda: settings.evaluate("!document.querySelector('[data-guidance=settings-grid] input').disabled"),
             bool, 'grid edit ready during mode refresh')
        settings.evaluate("document.querySelector('[data-guidance=settings-grid] input').click()")
        wait(environment, lambda value: value['grid']['enabled'] is not original, 'native grid write overtakes held read')
        main.evaluate('window.__environmentRace.release()')
        main.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        projected = main.evaluate(state)
        assert projected['gridEnabled'] is not original, f'Old environment sample rolled back grid UI: {projected}'
    finally:
        main.evaluate("""(async()=>{const p=window.__environmentRace;p.api.scene.readEnvironment=p.read;p.release?.();
          if(p.args)await corona.scene.setEnvironment(p.args[0],{...p.args[1],grid:{enabled:"""
                      + json.dumps(original) + "}});return true;})()")
        select_mode('Vision PT')
        wait(lambda: main.evaluate(state), lambda value: value['gridEnabled'] is original
             and value['renderMode'] == 'path_tracing', 'normal mode and grid restored after race')
    return dict(old_environment_projection_retired=True, live_dock_grid_edit=True)


def run(port, root):
    def targets():
        with urlopen(f"http://127.0.0.1:{port}/json/list", timeout=3) as response:
            return [p for p in json.load(response) if p["type"] == "page" and "/CabbageEditor/Frontend/" in p["url"]]

    main_targets = [target for target in targets() if 'uiOwner=' not in target['url'] and 'standalone=1' not in target['url']]
    assert len(main_targets) == 1, main_targets
    main = Frontend(port, target_id=main_targets[0]["id"])
    clients, opened, panel_targets = [main], [], []
    session = main.evaluate("corona.scene.readActive()")['archive_session']
    def environment():
        return main.evaluate("corona.scene.readEnvironment('scene.ini'," + json.dumps({'archive_session': session}) + ")")

    service = """(async()=>{const module=await import(document.querySelector('script[type=module]').src);
      return Object.values(module).find(value=>typeof value?.openNativePanel==='function');})()"""
    monitor = """(()=>{if(typeof corona.editor!=='undefined')throw Error('Generic native dispatch remains');return true;})()"""
    def open_panel(panel_id, route, selector):
        result = main.evaluate(f"{service}.then(service=>service.openNativePanel({{panelId:{json.dumps(panel_id)},routePath:{json.dumps(route)},width:600,height:800}}))")
        assert result.get('window_token'), result
        opened.append(panel_id)
        target = wait(lambda: next((p for p in targets() if 'standalone=1' in p['url'] and f'#/{route.lstrip("/")}?' in p['url']), None), bool, panel_id)
        panel_targets.append(target['id'])
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
        render_lifetime = main_render_retirement(main, controls)
        selection_lifetime, scene_tools = main_selection_retirement(main, open_panel, root, session)
        transform_baseline = main_transform_baseline_race(main, scene_tools, session)
        print('main transform baseline evidence: ' + json.dumps(transform_baseline), flush=True)
        environment_race = main_environment_projection_race(main, controls, settings, environment)

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
        saved = main.evaluate("corona.scene.save('scene.ini'," + json.dumps({'archive_session': session}) + ")")
        assert saved['status'] == 'saved' and saved['archive_session'] == session and saved['scene'] == 'scene.ini', saved
        assert saved['filepath'], saved
        # Reopening retires detached documents; inspect their monitors while alive.
        for client in clients:
            assert client.evaluate('typeof corona.editor') == 'undefined'
        old_scope = session
        reopened = main.evaluate('corona.project.openProject(' + json.dumps(str(root)) + ')')
        assert reopened['ok'] and 'data' not in reopened, reopened
        # Project replacement retires these tabs. Never close their obsolete
        # numeric IDs a second time from cleanup; verify document retirement.
        opened.clear()
        wait(lambda: {target['id'] for target in targets()}.intersection(panel_targets),
             lambda remaining: not remaining, 'old project panel documents retired')
        session = main.evaluate('corona.scene.readActive()')['archive_session']
        assert session != old_scope
        assert environment()['physics']['floor_y'] == 6
        rejected = main.evaluate("corona.scene.setEnvironment('scene.ini'," + json.dumps({
            'archive_session': old_scope, 'grid': {'enabled': original['grid']['enabled']},
        }) + ").catch(error=>error.code)")
        assert rejected == 'PROJECT_CONTEXT_CHANGED', rejected
        assert main.evaluate('typeof corona.editor') == 'undefined'
        return {'transform_baseline': transform_baseline, 'selection_lifetime': selection_lifetime, 'environment_projection_race': environment_race, 'main_render_lifetime': render_lifetime, 'documents': len(clients), 'uiWrites': ['sun.enabled', 'sun.direction', 'grid.enabled', 'physics.floor_y'],
                'samePhysicsGravity': True, 'saveReopenFloorY': environment()['physics']['floor_y'],
                'oldScopeRejected': rejected, 'retiredEnvironmentRequests': 0}
    finally:
        for panel_id in opened:
            main.evaluate(f'corona.window.close({{panelId:{json.dumps(panel_id)}}})')
        for client in clients:
            client.socket.close()
