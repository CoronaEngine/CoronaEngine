"""Retire the real packaged NodeGraph while its initial read/subscriptions wait."""
import time

from web_environment_smoke import wait


def run_camera(panel):
    """Deliver a real settings reply only after the production view unmounts."""
    panel.evaluate("""(async()=>{
      const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.readScene==='function');
      const app=Object.values(m).find(v=>typeof v?.openNativePanel==='function');
      const original={set:api.scene.updateCamera,resize:app.resizeThisCameraView};
      const probe=window.__cameraLifetime={resizes:[],pending:[],delivered:0,route:location.hash};
      api.scene.updateCamera=(...args)=>{
        const result=original.set(...args);
        return document.querySelector('.camera-overlay')&&'move_speed' in args[2]
          ?Promise.resolve(result).then(value=>new Promise(resolve=>probe.pending.push(()=>{probe.delivered++;resolve(value);})))
          :result;
      };
      app.resizeThisCameraView=(...args)=>{probe.resizes.push(args);return original.resize(...args);};
      probe.restore=()=>{for(const deliver of probe.pending.splice(0))deliver();
        api.scene.updateCamera=original.set;app.resizeThisCameraView=original.resize;};
      return true;
    })()""")
    try:
        panel.evaluate("""(()=>{const input=document.querySelector('.resolution input');
          if(!input)throw Error('Camera resolution control missing');
          input.value=384;input.dispatchEvent(new Event('input',{bubbles:true}));
          input.dispatchEvent(new Event('change',{bubbles:true}));return true;})()""")
        wait(lambda: panel.evaluate('window.__cameraLifetime.pending.length'), lambda n: n == 1,
             'real camera settings result waiting for its consumer')
        panel.evaluate("location.hash='/SceneBar?'+location.hash.split('?')[1];true")
        wait(lambda: panel.evaluate("!document.querySelector('.camera-overlay')&&!!document.querySelector('.scene-tools-panel')"),
             bool, 'CameraView route actually unmounted before settings reply')
        panel.evaluate('for(const deliver of window.__cameraLifetime.pending.splice(0))deliver();true')
        # A separate evaluation runs after the async save continuation's microtasks.
        result = panel.evaluate('({resizes:window.__cameraLifetime.resizes,delivered:window.__cameraLifetime.delivered})')
        assert result['delivered'] == 1, result
        assert result['resizes'] == [], 'retired CameraView resized its native window: ' + repr(result)
    finally:
        panel.evaluate('window.__cameraLifetime.restore();location.hash=window.__cameraLifetime.route;true')
    wait(lambda: panel.call('Runtime.evaluate', {
        'expression': "Boolean(document.querySelector('.camera-overlay')&&getEventListeners(window).keydown?.some(e=>e.listener.toString().includes('KeyW')))",
        'includeCommandLineAPI': True, 'returnByValue': True,
    })['result'].get('value'), bool, 'camera remount installs input normally')
    initialization = run_camera_initialization(panel)
    return dict(initialization=initialization, settingsReplyRetired=True, nativeResizeAfterRetirement=0, normalRemount=True,
                nativeDocumentPreserved=True)


def run_camera_initialization(panel):
    panel.evaluate("""(async()=>{
      const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.readScene==='function');
      const app=Object.values(m).find(v=>typeof v?.openNativePanel==='function');
      const runtime=window.__coronaObserverRuntime||Object.values(m).find(v=>typeof v?.observe==='function'&&typeof v?.off==='function'&&typeof v?.start==='function');
      if(!runtime)throw Error('observer runtime export is unavailable');
      const original={read:api.scene.readScene,observe:runtime.observe,off:runtime.off};
      const probe=window.__cameraInit={route:location.hash,phase:null};
      const camera=()=>!!document.querySelector('.camera-overlay');
      const hold=(kind,result)=>{
        const p=probe.phase;
        return p&&camera()&&p.kind===kind
          ?Promise.resolve(result).then(value=>new Promise(resolve=>p.pending.push(()=>resolve(value)))):result;
      };
      api.scene.readScene=(...args)=>{if(probe.phase&&camera())probe.phase.reads++;
        return hold('read',original.read(...args));};
      runtime.observe=(...args)=>{
        if(!probe.phase||!camera())return original.observe(...args);
        const p=probe.phase;p.registrations++;
        return hold('subscription',Promise.resolve(original.observe(...args)).then(token=>{p.token=token;return token;}));
      };
      runtime.off=token=>{if(probe.phase?.token===token)probe.phase.releases++;return original.off(token);};
      probe.arm=kind=>{probe.phase={kind,pending:[],reads:0,registrations:0,releases:0,token:null};};
      probe.release=()=>{for(const resume of probe.phase.pending.splice(0))resume();};
      probe.snapshot=()=>{const p=probe.phase;return {kind:p.kind,pending:p.pending.length,reads:p.reads,
        registrations:p.registrations,releases:p.releases,token:p.token};};
      probe.restore=()=>{probe.release();probe.phase=null;
        api.scene.readScene=original.read;runtime.observe=original.observe;runtime.off=original.off;};
      return true;
    })()""")
    results = []
    try:
        for kind in ('read', 'subscription'):
            panel.evaluate("location.hash='/SceneBar?'+location.hash.split('?')[1];true")
            wait(lambda: panel.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('.camera-overlay')"),
                 bool, 'camera baseline unmounted')
            panel.evaluate(f"window.__cameraInit.arm('{kind}');location.hash=window.__cameraInit.route;true")
            pending = wait(lambda: panel.evaluate('window.__cameraInit.snapshot()'),
                           lambda p: p['pending'] == 1, 'camera initialization waiting at ' + kind)
            panel.evaluate("location.hash='/SceneBar?'+location.hash.split('?')[1];true")
            wait(lambda: panel.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('.camera-overlay')"),
                 bool, 'camera unmounted during ' + kind)
            panel.evaluate('window.__cameraInit.release();true')
            if kind == 'subscription':
                wait(lambda: panel.evaluate('window.__cameraInit.snapshot()'),
                     lambda p: p['releases'] == 1, 'late camera subscription released once')
            settled = panel.evaluate('window.__cameraInit.snapshot()')
            assert settled['reads'] == pending['reads'], settled
            assert settled['registrations'] == pending['registrations'], settled
            assert settled['releases'] == (1 if kind == 'subscription' else 0), settled
            installed = panel.call('Runtime.evaluate', {
                'expression': "Boolean(getEventListeners(window).keydown?.some(e=>e.listener.toString().includes('KeyW')))",
                'includeCommandLineAPI': True, 'returnByValue': True,
            })['result'].get('value')
            assert not installed, 'retired camera installed movement listeners after ' + kind
            results.append(settled)
    finally:
        panel.evaluate('window.__cameraInit.restore();location.hash=window.__cameraInit.route;true')
    wait(lambda: panel.call('Runtime.evaluate', {
        'expression': "Boolean(document.querySelector('.camera-overlay')&&getEventListeners(window).keydown?.some(e=>e.listener.toString().includes('KeyW')))",
        'includeCommandLineAPI': True, 'returnByValue': True,
    })['result'].get('value'), bool, 'normal camera remount after initialization retirement')
    return results


def run_node_graph(panel):
    panel.evaluate("window.__lifetimeQuery=location.hash.split('?')[1]||'';location.hash='/SceneBar?'+window.__lifetimeQuery;true")
    wait(lambda: panel.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'lifecycle baseline route')
    panel.evaluate("""(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      const api=Object.values(m).find(v=>typeof v?.scene?.readScene==='function');
      const runtime=window.__coronaObserverRuntime||Object.values(m).find(v=>typeof v?.observe==='function'&&typeof v?.off==='function'&&typeof v?.start==='function');
      if(!runtime)throw Error('observer runtime export is unavailable');
      const original={read:api.scene.readActive,observe:runtime.observe,off:runtime.off,
        load:api.projectFiles.load,tree:api.scene.readScene,resize:window.ResizeObserver};
      const probe=window.__viewLifetime={phase:null};
      api.scene.readActive=(...args)=>{const phase=probe.phase;
        const hold=phase&&document.querySelector('.node-graph-workspace');
        const result=original.read(...args);
        return hold?Promise.resolve(result).then(value=>new Promise(resolve=>phase.reads.push(()=>resolve(value)))):result;};
      runtime.observe=(kind,callback)=>{const phase=probe.phase;
        if(!phase||!document.querySelector('.node-graph-workspace'))return original.observe(kind,callback);
        const entry={kind,callback,token:null,releases:0};phase.subscriptions.push(entry);
        return Promise.resolve(original.observe(kind,callback)).then(token=>{
          entry.token=token;
          return kind==='tree'?new Promise(resolve=>{entry.release=()=>resolve(token);}):token;
        });};
      runtime.off=token=>{for(const entry of probe.phase?.subscriptions||[])if(entry.token===token)entry.releases++;
        return original.off(token);};
      api.projectFiles.load=(...args)=>{if(probe.phase)probe.phase.loads++;return original.load(...args);};
      api.scene.readScene=(...args)=>{if(probe.phase&&args[0]?.actors==='context')probe.phase.actorReads++;return original.tree(...args);};
      window.ResizeObserver=class extends original.resize{
        constructor(callback){super(callback);this.phase=probe.phase&&document.querySelector('.node-graph-workspace')?probe.phase:null;
          if(this.phase)this.phase.observers++;}
        disconnect(){if(this.phase){this.phase.disconnected++;this.phase=null;}return super.disconnect();}
      };
      probe.arm=()=>{probe.phase={reads:[],subscriptions:[],loads:0,actorReads:0,observers:0,disconnected:0};};
      probe.release=()=>{const phase=probe.phase;
        for(const resume of phase.reads.splice(0))resume();
        for(const entry of phase.subscriptions){entry.release?.();entry.callback({scene:'scene.ini',archive_session:'retired'});}
      };
      probe.read=()=>{const p=probe.phase;return {pendingReads:p.reads.length,loads:p.loads,actorReads:p.actorReads,
        observers:p.observers,disconnected:p.disconnected,
        subscriptions:p.subscriptions.map(({kind,token,releases})=>({kind,token,releases}))};};
      probe.restore=()=>{if(probe.phase)probe.release();probe.phase=null;api.scene.readActive=original.read;
        runtime.observe=original.observe;runtime.off=original.off;api.projectFiles.load=original.load;
        api.scene.readScene=original.tree;window.ResizeObserver=original.resize;};
      return true;})()""")
    results = []
    try:
        for cycle in range(3):
            panel.evaluate("window.__viewLifetime.arm();location.hash='/NodeGraph?'+window.__lifetimeQuery;true")
            pending = wait(lambda: panel.evaluate('window.__viewLifetime.read()'),
                           lambda p: p['pendingReads'] > 0 and len(p['subscriptions']) == 2
                           and all(e['token'] is not None for e in p['subscriptions']),
                           'initial graph read and delayed tree registration')
            assert pending['loads'] == 0 and pending['actorReads'] == 0, pending
            panel.evaluate("location.hash='/SceneBar?'+window.__lifetimeQuery;true")
            wait(lambda: panel.evaluate("!document.querySelector('.node-graph-workspace')&&!!document.querySelector('.scene-tools-panel')"),
                 bool, 'real Vue graph unmounted during initialization')
            retired = panel.evaluate('window.__viewLifetime.read()')
            assert {e['kind']: e['releases'] for e in retired['subscriptions']} == {'selection': 1, 'tree': 0}, retired
            assert retired['observers'] > 0 and retired['disconnected'] == retired['observers'], retired
            panel.evaluate('window.__viewLifetime.release();true')
            settled = wait(lambda: panel.evaluate('window.__viewLifetime.read()'),
                           lambda p: all(e['releases'] == 1 for e in p['subscriptions']),
                           'late tree token released exactly once')
            time.sleep(.4)  # Allow a stale callback's ordinary refresh timer to expose itself.
            settled = panel.evaluate('window.__viewLifetime.read()')
            assert settled['loads'] == 0 and settled['actorReads'] == 0, settled
            assert all(e['releases'] == 1 for e in settled['subscriptions']), settled
            results.append(dict(cycle=cycle, **settled))
    finally:
        panel.evaluate('window.__viewLifetime.restore();true')
    panel.evaluate("location.hash='/NodeGraph?'+window.__lifetimeQuery;true")
    wait(lambda: panel.evaluate("!!document.querySelector('.ng-node[data-node-id=packaged-start]')"),
         bool, 'graph remounts normally after lifecycle races')
    return dict(cycles=results, normalRemount=True, nativeDocumentPreserved=True,
                scope='real Vue route unmount in one live CEF Dock; deferred facade replies')
