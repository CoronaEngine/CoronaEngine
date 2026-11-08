"""Exercise packaged graph targets, structured document persistence and run cleanup."""
import hashlib
import json
from pathlib import Path
import sys
import time
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from vision_frontend_probe import Frontend
from web_environment_smoke import wait
from web_view_lifetime_smoke import run_node_graph

DOCK_STORE = "document.querySelector('#app').__vue_app__.config.globalProperties.$pinia._s.get('dock')"


def open_graph(main, port, targets, description):
    wait(targets, lambda values: not any('#/NodeGraph?' in value['url'] for value in values),
         'previous graph document retired')
    main.evaluate(f"{DOCK_STORE}.openPanel('NodeGraphPanel');true")
    target = wait(lambda: next((target for target in targets() if '#/NodeGraph?' in target['url']
                               and 'uiOwner=' in target['url']), None), bool, description)
    panel = Frontend(port, target_id=target['id'])
    identity = panel.evaluate('corona.window.getCurrent()')
    return panel, identity['tabId'], target


def verify_close_during_save(main, panel, tab_id, target_id, service, targets, scope):
    """Delay/fail a facade save while real Vue edits and native Dock closure run."""
    panel.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.readScene==='function');
      const original=api.projectFiles.save;
      const probe=window.__closeSave={requests:[],pending:[]};
      api.projectFiles.save=request=>new Promise((resolve,reject)=>{
        probe.requests.push(structuredClone(request));
        probe.pending.push({request,resolve,reject});
      });
      probe.release=success=>{const item=probe.pending.shift();if(!item)throw Error('No pending save');
        if(success)original(item.request).then(item.resolve,item.reject);
        else item.reject(Object.assign(Error('Injected graph save failure'),{code:'SAVE_FAILED'}));};
      probe.restore=()=>{api.projectFiles.save=original;
        for(const item of probe.pending.splice(0))item.reject(Error('Probe cleanup'));};
      return true;})()""")
    main.evaluate(service + """.then(s=>{const original=s.closePanelTab;window.__originalGraphClose=original;
      s.closePanelTab=(id,panelId)=>{if(panelId!=='NodeGraphPanel')return original(id,panelId);
        window.__graphClose={status:'pending',tabId:id};return original(id,panelId).then(
          result=>{window.__graphClose={status:'closed',tabId:id};return result;},
          error=>{window.__graphClose={status:'failed',code:error.code,message:error.message,tabId:id};throw error;});};
      return true;})""")

    def drag_node(delta):
        panel.evaluate("""(()=>{const node=document.querySelector('.ng-node[data-node-id=packaged-start]');
          const r=node.getBoundingClientRect(),x=r.left+12,y=r.top+12;
          node.dispatchEvent(new MouseEvent('mousedown',{bubbles:true,button:0,clientX:x,clientY:y}));
          window.dispatchEvent(new MouseEvent('mousemove',{buttons:1,clientX:x+""" + str(delta)
                       + """,clientY:y}));
          window.dispatchEvent(new MouseEvent('mouseup',{button:0,clientX:x+""" + str(delta)
                       + """,clientY:y}));return true;})()""")

    drag_node(37)
    first = wait(lambda: panel.evaluate('window.__closeSave.requests'), bool,
                 'real Vue drag starts the delayed graph save')
    assert len(first) == 1 and first[0]['document']['workspace']['nodes'][0]['x'] > 80, first
    drag_node(41)
    time.sleep(.5)  # The second edit's 300 ms debounce must reach the in-flight save.
    expected_x = panel.evaluate("parseFloat(document.querySelector('.ng-node[data-node-id=packaged-start]').style.left)")
    assert expected_x > first[0]['document']['workspace']['nodes'][0]['x'], (expected_x, first)

    def close():
        main.evaluate(f"window.__graphClose={{status:'pending'}};{DOCK_STORE}.closePanel('NodeGraphPanel');true")

    close()
    time.sleep(.5)
    assert main.evaluate('window.__graphClose')['status'] == 'pending', 'Dock closed before its pending graph save'
    assert any(target['id'] == target_id for target in targets()), 'saving graph document was destroyed'
    panel.evaluate('window.__closeSave.release(false);true')
    failed = wait(lambda: main.evaluate('window.__graphClose'),
                  lambda value: value['status'] != 'pending', 'graph save failure rejects close')
    assert failed['status'] == 'failed' and failed['code'] == 'GRAPH_SAVE_FAILED', failed
    assert failed['tabId'] == tab_id, failed
    time.sleep(.5)
    assert panel.evaluate('window.__closeSave.requests.length') == 1, 'failed saves must wait for explicit retry'
    assert panel.evaluate("parseFloat(document.querySelector('.ng-node[data-node-id=packaged-start]').style.left)") == expected_x
    before = main.evaluate('corona.projectFiles.load(' + json.dumps(scope) + ')')
    assert before['document']['workspace']['nodes'][0]['x'] == 80, before

    # The real menu must reuse the document whose close failed, then retry it.
    main.evaluate(f"{DOCK_STORE}.openPanel('NodeGraphPanel');true")
    main.evaluate("new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))")
    assert [target['id'] for target in targets() if '#/NodeGraph?' in target['url']] == [target_id]
    close()
    retried = wait(lambda: panel.evaluate('window.__closeSave.requests'), lambda value: len(value) == 2,
                   'explicit Dock close retries the retained dirty graph')
    assert retried[1]['document']['workspace']['nodes'][0]['x'] == expected_x, retried
    assert main.evaluate('window.__graphClose')['status'] == 'pending'
    panel.evaluate('window.__closeSave.release(true);true')
    closed = wait(lambda: main.evaluate('window.__graphClose'),
                  lambda value: value['status'] != 'pending', 'saved graph Dock closes')
    assert closed['status'] == 'closed', closed
    wait(targets, lambda value: all(target['id'] != target_id for target in value),
         'native graph document retirement after saving')
    persisted = main.evaluate('corona.projectFiles.load(' + json.dumps(scope) + ')')
    assert persisted['document']['workspace']['nodes'][0]['x'] == expected_x, persisted
    main.evaluate(service + '.then(s=>{s.closePanelTab=window.__originalGraphClose;delete window.__originalGraphClose;return true;})')
    return dict(pendingCloseBlocked=True, failedClose=failed, automaticRetries=0,
                saveAttempts=len(retried), latestNodeX=expected_x, savedRevision=persisted['revision'],
                productionPanelId='NodeGraphPanel', menuRetryReusesDocument=True)


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
        panel, tab_id, target = open_graph(main, port, targets, 'node graph document')
        wait(lambda: panel.evaluate("!!document.querySelector('select[aria-label=运行目标] option[value=Probe]')"),
             bool, 'node graph Actor options')
        panel.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.scene?.readScene==='function');
          if(!api)throw Error('Shared actor index facade missing');
          window.__actorIndexes=[];window.__oldSnapshots=[];
          api.scene.getSnapshot=()=>{window.__oldSnapshots.push('snapshot');throw Error('Full scene snapshot forbidden for target validation');};
          const read=api.scene.readScene;
          api.scene.readScene=(...args)=>read(...args).then(result=>{if(args[0]?.actors==='index')window.__actorIndexes.push({args,result});return result;});
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
            assert call['args'] == [{'scene': 'scene.ini', 'archive_session': session, 'actors': 'index'}], call
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
        main.evaluate(f'{service}.then(s=>s.closePanelTab({tab_id},"NodeGraphPanel"))')
        tab_id = None
        stopped = wait(lambda: main.evaluate('corona.scripts.getStatus()'),
                       lambda value: value.get('status') == 'stopped' and not value.get('threadAlive')
                       and not value.get('snapshotCaptured') and not value.get('inputLocked'),
                       'closing the graph panel stops and restores its run')
        assert stopped.get('restoreStatus') == 'restored' and not stopped.get('restoreError'), stopped
        panel.socket.close()
        panel = None
        panel, tab_id, target = open_graph(main, port, targets, 'reopened graph document')
        wait(lambda: panel.evaluate("!!document.querySelector('.ng-node[data-node-id=packaged-start]')"),
             bool, 'saved graph survives panel closure and reopening')
        close_save = verify_close_during_save(main, panel, tab_id, target['id'], service, targets, scope)
        tab_id = None
        panel.socket.close()
        panel = None
        panel, tab_id, target = open_graph(main, port, targets, 'graph reopened after pending save')
        wait(lambda: panel.evaluate("parseFloat(document.querySelector('.ng-node[data-node-id=packaged-start]')?.style.left)"),
             lambda value: value == close_save['latestNodeX'], 'latest dirty edit survives native close and reopen')
        close_save['reopenedLatestEdit'] = True
        lifetime = run_node_graph(panel)
        return {'documents': 4, 'archiveSession': session, 'targetChanges': ['Probe', 'project'],
                'scopedIndexReads': len(calls), 'fullSnapshotReads': 0,
                'structuredDocumentCalls': file_calls, 'savedRevision': persisted['revision'],
                'uiRunId': running['runId'], 'closeRestore': stopped['restoreStatus'], 'reopenedGraph': True,
                'closeDuringSave': close_save, 'viewLifetime': lifetime}
    finally:
        if panel:
            try:
                panel.evaluate('window.__closeSave?.restore();true')
            except Exception:
                pass  # The native document may already be retired.
        main.evaluate(service + '.then(s=>{if(window.__originalGraphClose)s.closePanelTab=window.__originalGraphClose;return true;})')
        if tab_id is not None:
            main.evaluate(f'{service}.then(s=>s.closePanelTab({tab_id},"NodeGraphPanel"))')
        if panel:
            panel.socket.close()
        main.socket.close()
