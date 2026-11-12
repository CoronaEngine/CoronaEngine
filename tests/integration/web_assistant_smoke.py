"""Exercise real assistant task/chat documents against the packaged main owner."""
import json
from pathlib import Path
from urllib.parse import urlsplit
from urllib.request import url2pathname, urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def verify_startup(main, root, service, assistant, open_panel, close_panel, select):
    """Reload the real main document; delay only the Web facade's native result."""
    main.evaluate("location.hash='/SceneBar';true")
    wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('[data-viewport-pick-surface]')"),
         bool, 'startup fixture main view unmounted')
    active = main.evaluate('corona.scene.readActive()')
    scope = dict(project_path=str(root), archive_session=active['archive_session'], document='CabbageAssistant/context.json')
    saved = dict(schemaVersion=2, worldId=root.name, updatedAt=1,
                 worldGoal=dict(source='ai', status='ready'),
                 activeTasks=[dict(taskKey='goal.saved-first', title='Saved first task', type='goal', status='active', order=1),
                              dict(taskKey='goal.saved-second', title='Saved second task', type='goal', status='active', order=2)],
                 chatMessages=[dict(id='saved-message', role='user', content='Persisted assistant message')])
    previous = main.evaluate('corona.projectSettings.readProjectDocument(' + json.dumps(scope) + ')')
    committed = main.evaluate('corona.projectSettings.commitProjectDocument('
                              + json.dumps(dict(scope, revision=previous['revision'], content=saved)) + ')')
    assert committed.get('success'), committed
    seed = main.evaluate("JSON.parse(localStorage.getItem('corona.cabbageAssistantContext.v2'))")
    assert seed and seed.get('projectScopeId'), seed
    seed.update(activeTasks=[dict(taskKey='goal.recovery-cache', title='Recovery cache task', type='goal', status='active')],
                chatMessages=[dict(id='recovery-message', role='user', content='Recovery cache message')],
                selectedTaskKey='goal.recovery-cache', updatedAt=100)
    main.evaluate("localStorage.setItem('corona.cabbageAssistantContext.v2'," + json.dumps(json.dumps(seed)) + ');true')
    # The startup probe deliberately replaces the entire main CEF document.
    # Retire its registered Dock session first, as production world transitions
    # do; preserving old native windows across a forced reload is a separate
    # lifecycle and is not the supported generation replacement under test.
    retired = main.evaluate(f'{service}.then(s=>s.setEditorUiEnabled(false))')
    assert retired.get('enabled') is False, retired
    main.call('Page.reload', {'ignoreCache': True})

    def ready():
        try:
            return main.evaluate("!!document.querySelector('.scene-tools-panel')")
        except Exception:
            return False  # Page reload destroys the previous execution context.

    wait(ready, bool, 'new main document on startup route', seconds=60)
    dist = Path(url2pathname(urlsplit(main.evaluate('location.href')).path)).parent
    bundles = list((dist / 'assets').glob('cabbageAssistantContextService-*.js'))
    assert len(bundles) == 1, bundles
    main.evaluate('window.__assistantDomainUrl=' + json.dumps(bundles[0].as_uri()))
    main.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.projectSettings?.readProjectDocument==='function');
      const domain=await import(window.__assistantDomainUrl);
      const ai=Object.values(domain).find(v=>typeof v?.loadCabbageContext==='function');
      if(!api||!ai)throw Error('Assistant/project facade missing');
      const original=api.projectSettings.readProjectDocument,load=ai.loadCabbageContext;
      const p=window.__assistantStartup={completedLoads:0,pending:[],released:false};
      api.projectSettings.readProjectDocument=async request=>{
        const result=await original(request);
        if(request.document==='CabbageAssistant/context.json')p.savedTimestamp??=result.content.updatedAt;
        return result;};
      ai.loadCabbageContext=async request=>{
        const result=await load(request);
        if(p.released)return result;
        p.completedLoads++;p.result=structuredClone(result);
        return new Promise(resolve=>p.pending.push(()=>resolve(result)));};
      p.release=()=>{p.released=true;for(const done of p.pending.splice(0))done();
        return new Promise(resolve=>setTimeout(()=>{const s=document.querySelector('#app').__vue_app__.config.globalProperties.$pinia._s.get('cabbageAssistant');
          p.afterDelivery={tasks:s.activeTasks.map(t=>t.taskKey),selected:s.selectedTaskKey};resolve(true);},0));};
      p.restore=()=>{p.release();api.projectSettings.readProjectDocument=original;
        ai.loadCabbageContext=load;};return true;})()""")
    try:
        main.evaluate("location.hash='/';true")
        wait(lambda: main.evaluate('window.__assistantStartup.completedLoads'), lambda v: v >= 1, 'real startup workflow result pending')
        recovered = main.evaluate(f"(()=>{{const s={assistant};return {{tasks:s.activeTasks.map(t=>t.taskKey),"
                                  "selected:s.selectedTaskKey,messages:s.messages.map(m=>m.content)};})()")
        assert recovered == dict(tasks=['goal.recovery-cache'], selected='goal.recovery-cache',
                                 messages=['Recovery cache message']), recovered
        assert main.evaluate('window.__assistantStartup.savedTimestamp') == 1
        assert main.evaluate('window.__assistantStartup.result.success') is True
        completed_at = main.evaluate('window.__assistantStartup.result.context.updatedAt')
        # A real UI action overtakes the completed backend transaction while
        # its result is still awaiting delivery to the mounted MainPage.
        recovery_chat = open_panel('CabbageChatPanel', '.context-select')
        wait(lambda: recovery_chat.evaluate(f'{assistant}.selectedTaskKey'), lambda v: v == 'goal.recovery-cache',
             'startup chat joins the recovery snapshot')
        select(recovery_chat, '')
        wait(lambda: main.evaluate(f'({{selected:{assistant}.selectedTaskKey,updatedAt:{assistant}.contextUpdatedAt}})'),
             lambda v: v['selected'] == '' and v['updatedAt'] > completed_at,
             'real UI publication overtakes completed startup transaction')
        main.evaluate('window.__assistantStartup.release()')
        expected = [task['taskKey'] for task in saved['activeTasks']]
        delivered = main.evaluate('window.__assistantStartup.afterDelivery')
        assert delivered == dict(tasks=expected, selected=''), delivered
        messages = main.evaluate(f'{assistant}.messages.map(m=>m.content)')
        assert messages == ['Persisted assistant message'], messages
        close_panel(recovery_chat)
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda v: v and v.get('enabled'), 'reloaded assistant owner ready')
        assert main.evaluate(f'{assistant}.selectedTaskKey') == ''
        return dict(realDocumentReload=True, previousSessionRetiredBeforeReload=True,
                    recoveryCacheHydrated=True, persistedInitialUpdatedAt=1,
                    completedBackendUpdatedAt=completed_at, newerVueSelectionRetained=True,
                    authoritativeTaskKeys=expected, restoredMessages=messages)
    finally:
        main.evaluate('window.__assistantStartup?.restore();true')


def verify_initial_retirement(main, service, assistant, open_panel, close_panel):
    results = []
    for panel, route, selector in [('CabbageTasks', '/CabbageTasks', '.cabbage-tasks-panel'),
                                   ('CabbageChatPanel', '/CabbageChat', '.context-select'),
                                   ('NodeGraphPanel', '/NodeGraph', '.node-graph-workspace')]:
        main.evaluate("""(()=>{const original=BroadcastChannel.prototype.postMessage;
          const p=window.__assistantInitialRead={pending:[],replies:0,released:false};
          BroadcastChannel.prototype.postMessage=function(value){
            if(value?.kind==='reply'&&Array.isArray(value.payload?.activeTasks)&&value.payload.projectScopeId){
              p.replies++;if(!p.released){const channel=this;
                p.pending.push(()=>original.call(channel,value));return;}}
            return original.call(this,value);};
          p.release=()=>{p.released=true;for(const reply of p.pending.splice(0))reply();};
          p.restore=()=>{p.release();BroadcastChannel.prototype.postMessage=original;};return true;})()""")
        client = None
        try:
            client = open_panel(panel, selector)
            wait(lambda: main.evaluate('window.__assistantInitialRead.pending.length'), lambda v: v >= 1,
                 f'{panel} initial owner snapshot reply held')
            client.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())')
            assert main.evaluate('window.__assistantInitialRead.pending.length') == 1, f'{panel} issued duplicate initial context reads'
            empty = client.evaluate(f'({{tasks:{assistant}.activeTasks.map(t=>t.taskKey),selected:{assistant}.selectedTaskKey}})')
            assert empty == dict(tasks=[], selected=''), empty
            identity = client.evaluate('corona.window.getCurrent()')['tabId']
            client.evaluate("window.__assistantQuery=location.hash.split('?')[1]||'';location.hash='/SceneBar?'+window.__assistantQuery;true")
            wait(lambda: client.evaluate(f"!document.querySelector({json.dumps(selector)})&&!!document.querySelector('.scene-tools-panel')"),
                 bool, f'{panel} real Vue unmounted before initial reply')
            main.evaluate('window.__assistantInitialRead.release();true')
            # The policy round trip uses the same owner channel after its held
            # context reply, so the retired continuation has actually settled.
            client.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())')
            retired = client.evaluate(f'({{tasks:{assistant}.activeTasks.map(t=>t.taskKey),selected:{assistant}.selectedTaskKey}})')
            assert retired == empty, retired
            client.evaluate('location.hash=' + json.dumps(route + '?') + '+window.__assistantQuery;true')
            wait(lambda: client.evaluate(f'{assistant}.activeTasks.length'), lambda v: v == 2,
                 f'{panel} remount requests and hydrates current owner')
            replies = main.evaluate('window.__assistantInitialRead.replies')
            assert replies == 2, f'Retired cache bypassed the new owner read: {replies}'
            assert client.evaluate('corona.window.getCurrent()')['tabId'] == identity
            results.append(dict(panel=panel, tabId=identity, initialReplyAfterUnmountIgnored=True,
                                remountOwnerReads=replies, documentPreserved=True))
        finally:
            main.evaluate('window.__assistantInitialRead.restore();true')
            if client is not None:
                close_panel(client)
    return results


def run(port, root):

    def targets():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
            return [p for p in json.load(response) if p['type'] == 'page'
                    and '/CabbageEditor/Frontend/' in p['url']]

    mains = [p for p in targets() if 'uiOwner=' not in p['url'] and 'standalone=1' not in p['url']]
    assert len(mains) == 1, mains
    main = Frontend(port, target_id=mains[0]['id'])
    clients, opened = [main], []
    service = """(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      return Object.values(m).find(v=>typeof v?.openNativePanel==='function');})()"""
    assistant = "document.querySelector('#app').__vue_app__.config.globalProperties.$pinia._s.get('cabbageAssistant')"
    guard = """(()=>{const p=Storage.prototype,g=p.getItem,s=p.setItem;
      window.__assistantStorage={reads:0,writes:0,restore:()=>{p.getItem=g;p.setItem=s;}};
      p.getItem=function(k){if(k==='corona.cabbageAssistantContext.v2'){
        window.__assistantStorage.reads++;throw Error('test storage unavailable');}return g.call(this,k);};
      p.setItem=function(k,v){if(k==='corona.cabbageAssistantContext.v2'){
        window.__assistantStorage.writes++;throw Error('test storage unavailable');}return s.call(this,k,v);};return true;})()"""

    def state(client):
        return client.evaluate(f"(()=>{{const s={assistant};return s?{{scope:s.projectScopeId,"
                               "tasks:s.activeTasks.map(t=>t.taskKey),selected:s.selectedTaskKey}:null;})()")

    def open_panel(panel, selector):
        before = {p['id'] for p in targets()}
        route = {'CabbageChatPanel': '/CabbageChat', 'NodeGraphPanel': '/NodeGraph'}.get(panel, '/' + panel)
        result = main.evaluate(f"{service}.then(s=>s.openNativePanel({{panelId:{json.dumps(panel)},"
                               f"routePath:{json.dumps(route)},width:600,height:750}}))")
        assert result.get('tab_id'), result
        opened.append(panel)
        target = wait(lambda: next((p for p in targets() if p['id'] not in before
                                   and f'#{route}?' in p['url']), None), bool, f'{panel} document')
        client = Frontend(port, target_id=target['id'])
        clients.append(client)
        wait(lambda: client.evaluate(f'!!document.querySelector({json.dumps(selector)})'), bool, f'{panel} mounted')
        return client

    def select(client, key):
        client.evaluate("(()=>{const s=document.querySelector('.context-select');s.value="
                        + json.dumps(key) + ";s.dispatchEvent(new Event('change',{bubbles:true}));return true;})()")

    def close_panel(client):
        panel = client.evaluate('corona.window.getCurrent()')['panelId']
        main.evaluate(f'{service}.then(s=>s.closePanelTab({json.dumps(panel)}))')
        opened.remove(panel)
        clients.remove(client)
        client.socket.close()

    try:
        main.evaluate("location.hash='/';true")
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda v: v and v.get('enabled'), 'main assistant owner ready')
        baseline = wait(lambda: state(main), lambda v: v and len(v['tasks']) >= 2,
                        'real world assistant tasks loaded', seconds=60)
        startup = verify_startup(main, root, service, assistant, open_panel, close_panel, select)
        retirement = verify_initial_retirement(main, service, assistant, open_panel, close_panel)
        baseline = state(main)
        main.evaluate(guard)
        board = open_panel('CabbageTasks', '.cabbage-tasks-panel')
        chat = open_panel('CabbageChatPanel', '.context-select')
        for client in (board, chat):
            wait(lambda: state(client), lambda v: v and v['scope'] == baseline['scope']
                 and v['tasks'] == baseline['tasks'], 'Dock hydrates owner tasks')
            client.evaluate(guard)
        options = chat.evaluate("[...document.querySelector('.context-select').options].map(o=>o.value).filter(Boolean)")
        assert options, options
        select(chat, options[0])
        for client in (main, board):
            wait(lambda: state(client), lambda v: v and v['selected'] == options[0],
                 'real chat selection reaches main and task board')
        late = open_panel('CabbageChatPanel', '.context-select')
        wait(lambda: state(late), lambda v: v and v['selected'] == options[0], 'late Dock hydrates active selection')
        late.evaluate(guard)
        identities = [client.evaluate('corona.window.getCurrent()')['tabId'] for client in (board, chat, late)]
        marker = main.evaluate('window.__assistantRemount=crypto.randomUUID()')
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('[data-viewport-pick-surface]')"),
             bool, 'MainPage actually unmounted')
        main.evaluate("location.hash='/';true")
        wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'MainPage actually remounted')
        assert main.evaluate('window.__assistantRemount') == marker
        wait(lambda: state(main), lambda v: v and v['selected'] == options[0], 'main remount retains active selection')
        select(late, '')
        for client in (main, board, chat):
            wait(lambda: state(client), lambda v: v and v['selected'] == '', 'reverse selection reaches surviving documents')
        assert [client.evaluate('corona.window.getCurrent()')['tabId'] for client in (board, chat, late)] == identities
        writes = main.evaluate('window.__assistantStorage.writes')
        assert writes >= 2, writes
        peer_storage = [client.evaluate('({reads:window.__assistantStorage.reads,writes:window.__assistantStorage.writes})')
                        for client in (board, chat, late)]
        assert all(s == dict(reads=0, writes=0) for s in peer_storage), peer_storage
        # Supported generation changes retire registered native Dock documents;
        # an old Dock is not expected to silently join another world session.
        previous_policy = main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())')
        old_targets = {p['id'] for p in targets() if p['id'] != mains[0]['id']}
        retired = main.evaluate(f'{service}.then(s=>s.setEditorUiEnabled(false))')
        assert retired.get('enabled') is False, retired
        wait(targets, lambda values: not any(p['id'] in old_targets for p in values),
             'registered native Dock documents retired before generation replacement')
        for client in clients[1:]:
            client.socket.close()
        clients[:] = [main]
        opened.clear()
        next_policy = main.evaluate(f'{service}.then(s=>s.setEditorUiEnabled(true))')
        assert next_policy.get('enabled') is True, next_policy
        assert next_policy['generation'] != previous_policy['generation']
        fresh = open_panel('CabbageTasks', '.cabbage-tasks-panel')
        wait(lambda: state(fresh), lambda v: v and v['tasks'] == baseline['tasks'],
             'fresh generation Dock hydrates the current owner')
        fresh_policy = fresh.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())')
        assert fresh_policy['enabled'] and fresh_policy['generation'] == next_policy['generation'], fresh_policy
        generation_retirement = dict(oldDockTabs=identities, oldDocumentsRetired=len(old_targets),
                                     generationChanged=True, newDockHydrated=True,
                                     newTabId=fresh.evaluate('corona.window.getCurrent()')['tabId'])
        close_panel(fresh)
        return dict(taskKeys=baseline['tasks'], projectScopeId=baseline['scope'],
                    startupRecovery=startup,
                    initialReplyRetirement=retirement,
                    nativeGenerationRetirement=generation_retirement,
                    initialHydration=True, bidirectionalVueSelection=True, lateDockHydration=True,
                    selectedTaskKey='', documents=1 + len(identities), mainViewRemount=True,
                    survivingTabIds=identities, rejectedOwnerStorageWrites=writes, peerActiveStorageAccess=peer_storage)
    finally:
        try:
            for client in clients:
                try:
                    client.evaluate('window.__assistantStorage?.restore();true')
                except Exception:
                    pass
            for panel in opened:
                main.evaluate(f'corona.window.close({{panelId:{json.dumps(panel)}}})')
        finally:
            for client in clients:
                client.socket.close()
