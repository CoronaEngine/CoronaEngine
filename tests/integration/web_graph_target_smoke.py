"""Exercise packaged graph targets, structured document persistence and run cleanup."""
import hashlib
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
        main.evaluate("location.hash='/'")
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda policy: policy and policy.get('enabled'), 'graph Web owner ready')
        session = main.evaluate('corona.scene.readActive()')['archive_session']
        scope = dict(archive_session=session, project_path=str(root),
                     identity=['node_graph', 'project', '', ''])
        workspace = dict(version=1, nodes=[dict(id='packaged-start', nodeType='start',
                         macroType='state', name='开始', customName='状态1', x=80, y=100, workspace={})],
                         edges=[], globalVariablesWorkspace={})
        seed = dict(scope, code='# seed; the Run button must save generated code\n',
                    document=dict(target=dict(id='node_graph:project:global', script_kind='node_graph',
                                  target_type='project', scene_name='', actor_name='', enabled=True,
                                  runnable=True, validation_errors=[]), workspace=workspace))
        seeded = main.evaluate('corona.projectFiles.save(' + json.dumps(seed) + ')')
        assert seeded['status'] == 'saved', seeded
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
          window.__projectFiles=[];
          for(const name of ['save','load']){const original=api.projectFiles[name];
            api.projectFiles[name]=request=>original(request).then(result=>{
              window.__projectFiles.push({name,identity:request.identity,legacyKey:'key' in request,status:result.status});return result;});}
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
        wait(lambda: panel.evaluate("!!document.querySelector('.ng-node[data-node-id=packaged-start]')"),
             bool, 'persisted project graph loaded into the real UI')
        wait(lambda: panel.evaluate("!document.querySelector('button.ng-run').disabled"), bool, 'Run button enabled')
        panel.evaluate("document.querySelector('button.ng-run').click();true")
        running = wait(lambda: main.evaluate('corona.scripts.getStatus()'),
                       lambda value: value.get('marker', {}).get('id') == 'packaged-start'
                       and value.get('threadAlive') and value.get('snapshotCaptured'),
                       'Run button saves and starts the generated file')
        assert running['inputLocked'] and running['runId'], running
        persisted = main.evaluate('corona.projectFiles.load(' + json.dumps(scope) + ')')
        assert persisted['status'] == 'loaded', persisted
        assert persisted['document']['workspace']['nodes'][0]['id'] == 'packaged-start'
        code = (Path(root) / persisted['code_path']).read_bytes()
        assert hashlib.sha256(code).hexdigest() == persisted['revision']
        assert persisted['revision'] != seeded['revision'], 'Run must compile and publish the current UI graph'
        file_calls = panel.evaluate('window.__projectFiles')
        assert any(call['name'] == 'save' and call['status'] == 'saved' for call in file_calls), file_calls
        assert all(not call['legacyKey'] and len(call['identity']) == 4 for call in file_calls), file_calls
        main.evaluate(f'{service}.then(s=>s.closePanelTab({tab_id},"NodeGraph"))')
        tab_id = None
        stopped = wait(lambda: main.evaluate('corona.scripts.getStatus()'),
                       lambda value: value.get('status') == 'stopped' and not value.get('threadAlive')
                       and not value.get('snapshotCaptured') and not value.get('inputLocked'),
                       'closing the graph panel stops and restores its run')
        assert stopped.get('restoreStatus') == 'restored' and not stopped.get('restoreError'), stopped
        panel.socket.close()
        panel = None
        reopened = main.evaluate(f"{service}.then(s=>s.openNativePanel({{detached:true,panelId:'NodeGraph',routePath:'/NodeGraph',width:1100,height:800}}))")
        tab_id = reopened['tab_id']
        target = wait(lambda: next((target for target in targets() if '#/NodeGraph?' in target['url']
                                   and 'standalone=1' in target['url']), None), bool, 'reopened graph document')
        panel = Frontend(port, target_id=target['id'])
        wait(lambda: panel.evaluate("!!document.querySelector('.ng-node[data-node-id=packaged-start]')"),
             bool, 'saved graph survives panel closure and reopening')
        return {'documents': 3, 'archiveSession': session, 'targetChanges': ['Probe', 'project'],
                'scopedIndexReads': len(calls), 'fullSnapshotReads': 0,
                'structuredDocumentCalls': file_calls, 'savedRevision': persisted['revision'],
                'uiRunId': running['runId'], 'closeRestore': stopped['restoreStatus'], 'reopenedGraph': True}
    finally:
        if tab_id is not None:
            main.evaluate(f'{service}.then(s=>s.closePanelTab({tab_id},"NodeGraph"))')
        if panel:
            panel.socket.close()
        main.socket.close()
