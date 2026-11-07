"""Exercise packaged node graph target changes through the scoped Actor index."""
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
    selector = 'select[aria-label="运行目标"]'
    try:
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda policy: policy and policy.get('enabled'), 'graph Web owner ready')
        session = main.evaluate('corona.scene.readActive()')['archive_session']
        opened = main.evaluate(f"{service}.then(s=>s.openNativePanel({{detached:true,panelId:'NodeGraph',routePath:'/NodeGraph',width:1100,height:800}}))")
        tab_id = opened['tab_id']
        target = wait(lambda: next((target for target in targets() if '#/NodeGraph?' in target['url']
                                   and 'standalone=1' in target['url']), None), bool, 'node graph document')
        panel = Frontend(port, target_id=target['id'])
        wait(lambda: panel.evaluate("!!document.querySelector('select[aria-label=运行目标] option[value=Probe]')"),
             bool, 'node graph Actor options')
        panel.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.scene?.readActorIndex==='function');
          if(!api)throw Error('Shared actor index facade missing');
          window.__actorIndexes=[];window.__oldSnapshots=[];
          api.scene.getSnapshot=()=>{window.__oldSnapshots.push('snapshot');throw Error('Full scene snapshot forbidden for target validation');};
          const read=api.scene.readActorIndex;
          api.scene.readActorIndex=(...args)=>read(...args).then(result=>{window.__actorIndexes.push({args,result});return result;});
          return true;})()""")
        for actor in ['Probe', '']:
            panel.evaluate("(()=>{const input=document.querySelector(" + json.dumps(selector)
                           + ");input.value=" + json.dumps(actor)
                           + ";input.dispatchEvent(new Event('change',{bubbles:true}));return true;})()")
            wait(lambda: panel.evaluate('document.querySelector(' + json.dumps(selector) + ').value'),
                 lambda value: value == actor, f'node graph target {actor or "project"}')
        calls = panel.evaluate('window.__actorIndexes')
        assert len(calls) == 2, calls
        for call in calls:
            assert call['args'] == ['scene.ini', {'archive_session': session}], call
            assert call['result']['archive_session'] == session, call
            assert any(actor['name'] == 'Probe' for actor in call['result']['actors']), call
            assert 'cameras' not in call['result'], call
        assert panel.evaluate('window.__oldSnapshots') == []
        return {'documents': 2, 'archiveSession': session, 'targetChanges': ['Probe', 'project'],
                'scopedIndexReads': len(calls), 'fullSnapshotReads': 0}
    finally:
        if tab_id is not None:
            main.evaluate(f'{service}.then(s=>s.closePanelTab({tab_id},"NodeGraph"))')
        if panel:
            panel.socket.close()
        main.socket.close()
