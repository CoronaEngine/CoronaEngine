"""Verify the packaged Object Dock reads and reloads scoped typed properties."""
import json
from pathlib import Path
import sys
from urllib.parse import urlsplit
from urllib.request import url2pathname, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def alias_retirement(panel, main, scene, archive, guid):
    panel.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.updateActor==='function');
      if(!api)throw Error('Actor rename facade missing');
      const p=window.__aliasLifetime={api,rename:api.scene.updateActor,route:location.hash,reads:0,ready:false};
      api.scene.updateActor=(...args)=>p.rename(...args).then(result=>new Promise(resolve=>{
        p.release=()=>resolve(new Proxy(result,{get(target,key,receiver){
          if(key==='actor')p.reads++;return Reflect.get(target,key,receiver);}}));p.ready=true;}));
      const input=document.querySelector('#actor-alias');input.value='Retired alias probe';
      input.dispatchEvent(new Event('input',{bubbles:true}));
      input.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true,cancelable:true}));return true;})()""")
    try:
        wait(lambda: panel.evaluate('window.__aliasLifetime.ready'), bool, 'actual rename reply held')
        panel.evaluate("location.hash='/ProjectSettings'+window.__aliasLifetime.route.slice(window.__aliasLifetime.route.indexOf('?'))")
        wait(lambda: panel.evaluate("!!document.querySelector('.object-panel-shell')"), lambda value: not value,
             'Object rename view retired')
        panel.evaluate('window.__aliasLifetime.release()')
        panel.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        reads = panel.evaluate('window.__aliasLifetime.reads')
        assert reads == 0, f'Retired Object consumed {reads} rename reply fields'
    finally:
        panel.evaluate("(()=>{const p=window.__aliasLifetime;p.api.scene.updateActor=p.rename;p.release?.();})()")
        main.evaluate('corona.scene.updateActor(' + json.dumps(scene) + ',' + json.dumps(guid)
                      + ',' + json.dumps({'archive_session': archive, 'name': 'Probe'}) + ')')
        main.evaluate('corona.scene.selectActor(' + json.dumps(scene) + ',"actor","Probe")')
        panel.evaluate('location.hash=window.__aliasLifetime.route')
    wait(lambda: panel.evaluate("document.querySelector('.object-heading h2')?.textContent"),
         lambda name: name == 'Probe', 'Object normal remount after rename')
    return dict(late_rename_field_reads=reads, normal_remount=True)


def property_retirement(panel, main, scene, archive, guid):
    entry = panel.evaluate("document.querySelector('script[type=module]').src")
    chunks = list(Path(url2pathname(urlsplit(entry).path)).parent.glob('cabbageAssistantContextService-*.js'))
    assert len(chunks) == 1, chunks
    panel.evaluate('window.__physicsContextUrl=' + json.dumps(chunks[0].resolve().as_uri()))
    panel.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.updateActor==='function');
      const services=await import(window.__physicsContextUrl);
      const context=Object.values(services).find(v=>typeof v?.recordEvent==='function'&&typeof v?.notifySceneSaved==='function');
      if(!api||!context)throw Error('Actor property/context services missing');
      const p=window.__physicsLifetime={api,context,write:api.scene.updateActor,record:context.recordEvent,
        route:location.hash,events:[],ready:false,retired:false};
      context.recordEvent=(event,...args)=>{if(p.retired&&event.type==='physics_changed')p.events.push(event);
        return p.record(event,...args);};
      api.scene.updateActor=(...args)=>p.write(...args).then(result=>new Promise(resolve=>{
        p.release=()=>resolve(result);p.ready=true;}));
      [...document.querySelectorAll('#actor-body-type button')].find(b=>b.textContent.trim()==='静态').click();return true;})()""")
    try:
        wait(lambda: panel.evaluate('window.__physicsLifetime.ready'), bool, 'actual physics patch reply held')
        panel.evaluate("location.hash='/ProjectSettings'+window.__physicsLifetime.route.slice(window.__physicsLifetime.route.indexOf('?'))")
        wait(lambda: panel.evaluate("!!document.querySelector('.object-panel-shell')"), lambda value: not value,
             'Object physics view retired')
        panel.evaluate('window.__physicsLifetime.retired=true;window.__physicsLifetime.release()')
        panel.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))')
        events = panel.evaluate('window.__physicsLifetime.events')
        assert not events, f'Retired Object published physics events: {events}'
    finally:
        panel.evaluate("(()=>{const p=window.__physicsLifetime;p.api.scene.updateActor=p.write;"
                       "p.context.recordEvent=p.record;p.release?.();location.hash=p.route;})()")
    wait(lambda: panel.evaluate("document.querySelector('.object-heading h2')?.textContent"),
         lambda name: name == 'Probe', 'Object normal remount after physics patch')
    panel.evaluate("[...document.querySelectorAll('#actor-body-type button')].find(b=>b.textContent.trim()==='运动学').click()")
    request = json.dumps([scene, guid, {'archive_session': archive}])
    wait(lambda: main.evaluate(f'corona.scene.readActorProperties(...{request})'),
         lambda actor: actor['mechanics']['body_type'] == 'kinematic', 'live physics patch after remount')
    main.evaluate('corona.scene.updateActor(' + json.dumps(scene) + ',' + json.dumps(guid) + ','
                  + json.dumps({'archive_session': archive, 'mechanics': {'body_type': 'dynamic'}}) + ')')
    return dict(retired_physics_events=events, normal_remount=True, live_body_type_edit=True)


def run(port, root):
    def targets():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
            return [target for target in json.load(response) if target['type'] == 'page'
                    and '/CabbageEditor/Frontend/' in target['url']]

    main_targets = [target for target in targets() if 'uiOwner=' not in target['url']
                    and 'standalone=1' not in target['url']]
    assert len(main_targets) == 1, main_targets
    main = Frontend(port, target_id=main_targets[0]['id'])
    panel, tab_id = None, None
    service = """(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      return Object.values(m).find(v=>typeof v?.openNativePanel==='function');})()"""
    try:
        main.evaluate("location.hash='/'")
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda policy: policy and policy.get('enabled'), 'Object owner ready')
        scope = main.evaluate('corona.scene.readActive()')
        archive, scene = scope['archive_session'], scope['scene']
        arguments = json.dumps([scene, 'Probe', {'archive_session': archive}])

        def read():
            return main.evaluate(f'corona.scene.readActorProperties(...{arguments})')

        initial = read()
        main.evaluate(f'corona.scene.selectActor({json.dumps(scene)},"scene","")')
        opened = main.evaluate(f"{service}.then(s=>s.openNativePanel({{detached:true,panelId:'Object',routePath:'/Object',width:600,height:900}}))")
        tab_id = opened['tab_id']
        target = wait(lambda: next((target for target in targets() if '#/Object?' in target['url']
                                   and 'standalone=1' in target['url']), None), bool, 'Object Dock document')
        panel = Frontend(port, target_id=target['id'])
        wait(lambda: panel.evaluate("!!document.querySelector('.object-panel-shell')"), bool, 'Object mounted')
        panel.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.scene?.readActorProperties==='function');
          if(!api)throw Error('Actor property facade missing');
          window.__propertyReads=[];
          const read=api.scene.readActorProperties;
          api.scene.readActorProperties=(...args)=>read(...args).then(result=>{window.__propertyReads.push({args,result});return result;});
          if(typeof corona.editor!=='undefined')throw Error('Generic native dispatch remains');
          return true;})()""")
        main.evaluate(f'corona.scene.selectActor({json.dumps(scene)},"actor","Probe")')
        wait(lambda: panel.evaluate("document.querySelector('.object-heading h2')?.textContent"),
             lambda name: name == 'Probe', 'Object property projection applied')
        assert panel.evaluate("Number(document.querySelector('[data-guidance=object-position-x]').value)") == initial['geometry']['position'][0]
        for label, value in [('运动学', 'kinematic'), ('动态', 'dynamic')]:
            # Use the same actual DOM buttons as the user, including Vue handlers.
            panel.evaluate("[...document.querySelectorAll('#actor-body-type button')].find(b=>b.textContent.trim()==="
                           + json.dumps(label) + ').click()')
            wait(read, lambda actor: actor['mechanics']['body_type'] == value, f'Object {value} patch')
        panel.evaluate("""(()=>{const input=document.querySelector('[data-guidance=object-physics-mass]');
          input.value='3.5';input.dispatchEvent(new Event('input',{bubbles:true}));
          input.dispatchEvent(new Event('change',{bubbles:true}));return true;})()""")
        wait(read, lambda actor: actor['mechanics']['mass'] == 3.5, 'Object mass patch')
        main.evaluate(f'corona.scene.selectActor({json.dumps(scene)},"scene","")')
        wait(lambda: panel.evaluate("!document.querySelector('.object-heading')"), bool, 'Object selection cleared')
        main.evaluate(f'corona.scene.selectActor({json.dumps(scene)},"actor","Probe")')
        wait(lambda: panel.evaluate("document.querySelector('[data-guidance=object-physics-mass]')?.value"),
             lambda value: value == '3.5', 'Object reload preserves properties')
        calls = panel.evaluate('window.__propertyReads')
        assert len(calls) >= 2, calls
        for call in calls:
            assert call['args'] == [scene, initial['actor_guid'], {'archive_session': archive}], call
            result = call['result']
            assert result['archive_session'] == archive and result['scene'] == scene, result
            assert result['actor_guid'] == initial['actor_guid'] and isinstance(result['handle'], str), result
            assert 'world_aabb' not in result and 'material' not in result, result
        assert panel.evaluate('typeof corona.editor') == 'undefined'
        alias_lifetime = alias_retirement(panel, main, scene, archive, initial['actor_guid'])
        property_lifetime = property_retirement(panel, main, scene, archive, initial['actor_guid'])
        return {'archiveSession': archive, 'scopedPropertyReads': len(calls), 'legacyReads': 0,
                'bodyTypes': ['kinematic', 'dynamic'], 'reloadedMass': 3.5, 'opaqueHandle': True,
                'alias_lifetime': alias_lifetime, 'property_lifetime': property_lifetime}
    finally:
        if tab_id is not None:
            main.evaluate(f'{service}.then(s=>s.closePanelTab({tab_id},"Object"))')
        if panel:
            panel.socket.close()
        main.socket.close()
