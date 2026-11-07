"""Verify the packaged Object Dock reads and reloads scoped typed properties."""
import json
from pathlib import Path
import sys
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from vision_frontend_probe import Frontend
from web_environment_smoke import wait


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
        return {'archiveSession': archive, 'scopedPropertyReads': len(calls), 'legacyReads': 0,
                'bodyTypes': ['kinematic', 'dynamic'], 'reloadedMass': 3.5, 'opaqueHandle': True}
    finally:
        if tab_id is not None:
            main.evaluate(f'{service}.then(s=>s.closePanelTab({tab_id},"Object"))')
        if panel:
            panel.socket.close()
        main.socket.close()
