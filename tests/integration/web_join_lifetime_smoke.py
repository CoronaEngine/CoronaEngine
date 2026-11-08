"""Unmount the deployed JoinGame while real discovery results are waiting."""
import json
from urllib.request import urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def run(port, root):
    del root
    with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
        mains = [p for p in json.load(response) if p['type'] == 'page'
                 and '/CabbageEditor/Frontend/' in p['url'] and 'uiOwner=' not in p['url']]
    assert len(mains) == 1, mains
    main = Frontend(port, target_id=mains[0]['id'])
    results = []
    try:
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'discovery baseline route')
        main.evaluate("""(async()=>{
          const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.network?.searchLan==='function');
          const original={timeout:window.setTimeout,clear:window.clearTimeout,methods:{},create:api.project.createMultiplayerProject};
          const probe=window.__joinLifetime={phase:null};
          for(const name of ['clearDiscoveredPeers','searchLan','getDiscoveredPeers','getSessionInfo','stopSession']){
            original.methods[name]=api.network[name];
            api.network[name]=(...args)=>{
              const p=probe.phase;
              if(p?.kind==='room'&&p.retired&&name==='stopSession'){
                p.forbidden.push(name);return Promise.reject(Error('retired native operation blocked by acceptance probe'));
              }
              if(p){p.calls.push(name);p.inflight++;}
              const result=Promise.resolve(original.methods[name](...args)).finally(()=>{if(p)p.inflight--;});
              return p?.kind===name||(p?.kind==='room'&&name==='getSessionInfo')
                ?result.then(value=>new Promise(resolve=>p.pending.push(()=>resolve(value)))):result;
            };
          }
          api.project.createMultiplayerProject=(...args)=>{
            if(probe.phase?.kind==='room'&&probe.phase.retired){probe.phase.forbidden.push('createMultiplayerProject');
              return Promise.reject(Error('retired project creation blocked by acceptance probe'));}
            return original.create(...args);
          };
          window.setTimeout=(callback,ms,...args)=>{
            const p=probe.phase;
            if(!p||ms!==1500||!location.hash.startsWith('#/JoinGame'))return original.timeout.call(window,callback,ms,...args);
            const id=original.timeout.call(window,()=>{p.delays.delete(id);callback(...args);},ms);
            p.delays.add(id);return id;
          };
          window.clearTimeout=id=>{probe.phase?.delays.delete(id);return original.clear.call(window,id);};
          probe.arm=kind=>{probe.phase={kind,calls:[],inflight:0,pending:[],delays:new Set(),retired:false,forbidden:[]};};
          probe.release=()=>{for(const resume of probe.phase?.pending.splice(0)||[])resume();};
          probe.read=()=>{const p=probe.phase;return {kind:p.kind,calls:p.calls,inflight:p.inflight,
            pending:p.pending.length,delays:p.delays.size,forbidden:p.forbidden};};
          probe.turn=()=>new Promise(resolve=>original.timeout.call(window,()=>resolve(true),0));
          probe.restore=()=>{probe.release();probe.phase=null;
            for(const [name,method] of Object.entries(original.methods))api.network[name]=method;
            api.project.createMultiplayerProject=original.create;
            window.setTimeout=original.timeout;window.clearTimeout=original.clear;};
          return true;
        })()""")
        for kind in ('clearDiscoveredPeers', 'searchLan', 'delay'):
            main.evaluate('window.__joinLifetime.arm(' + json.dumps(kind) + ");location.hash='/JoinGame';true")
            pending = wait(lambda: main.evaluate('window.__joinLifetime.read()'),
                           lambda p: p['delays'] == 1 if kind == 'delay' else p['pending'] == 1,
                           'real JoinGame discovery waiting at ' + kind)
            assert main.evaluate("!!document.querySelector('input[placeholder=玩家名]')"), pending
            main.evaluate("location.hash='/SceneBar';true")
            wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('input[placeholder=玩家名]')"),
                 bool, 'actual JoinGame unmounted during ' + kind)
            main.evaluate('window.__joinLifetime.release();window.__joinLifetime.turn()')
            settled = wait(lambda: main.evaluate('window.__joinLifetime.read()'), lambda p: p['inflight'] == 0,
                           'released native discovery result settled')
            main.evaluate('window.__joinLifetime.turn()')
            settled = main.evaluate('window.__joinLifetime.read()')
            assert settled['calls'] == pending['calls'], 'retired JoinGame continued native work: ' + repr(settled)
            assert settled['delays'] == 0, 'retired JoinGame retained its discovery delay: ' + repr(settled)
            results.append(settled)
        room_retirement = []
        for role in ('host', 'guest'):
            main.evaluate("window.__joinLifetime.arm('mount');location.hash='/JoinGame';true")
            wait(lambda: main.evaluate("!!document.querySelector('input[placeholder=玩家名]')"), bool, 'room launcher mounted')
            wait(lambda: main.evaluate('window.__joinLifetime.read()'),
                 lambda p: 'getSessionInfo' in p['calls'] and p['inflight'] == 0,
                 'mounted launcher session info delivered')
            main.evaluate("window.__joinLifetime.arm('room');true")
            if role == 'host':
                main.evaluate("[...document.querySelectorAll('button')].find(b=>b.textContent.trim()==='创建房间').click();true")
                main.evaluate("(()=>{const input=document.querySelector('input[placeholder^=给你的房间]');input.value='Retirement probe';"
                              "input.dispatchEvent(new Event('input',{bubbles:true}));return true;})()")
                main.evaluate("[...document.querySelectorAll('button')].find(b=>b.textContent.trim()==='创建房间'&&!b.disabled&&b.classList.contains('px-14')).click();true")
            else:
                main.evaluate("(()=>{const input=document.querySelector('input[placeholder^=主机]');input.value='127.0.0.1';"
                              "input.dispatchEvent(new Event('input',{bubbles:true}));return true;})()")
                main.evaluate("[...document.querySelectorAll('button')].find(b=>b.textContent.trim()==='连接').click();true")
            wait(lambda: main.evaluate('window.__joinLifetime.read()'), lambda p: p['pending'] == 1,
                 role + ' actual session-info result awaiting its consumer')
            main.evaluate("location.hash='/SceneBar';true")
            wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('input[placeholder=玩家名]')"),
                 bool, 'room launcher unmounted before session-info delivery')
            main.evaluate('window.__joinLifetime.phase.retired=true;window.__joinLifetime.release();window.__joinLifetime.turn()')
            settled = main.evaluate('window.__joinLifetime.read()')
            assert settled['forbidden'] == [], 'retired room action continued: ' + repr(settled)
            assert settled['delays'] == 0, settled
            room_retirement.append(dict(role=role, **settled))
        main.evaluate("window.__joinLifetime.phase=null;location.hash='/JoinGame';true")
        wait(lambda: main.evaluate("!!document.querySelector('input[placeholder=玩家名]')&&!document.querySelector('svg.animate-spin')"),
             bool, 'normal JoinGame remount completes discovery')
        return dict(roomRetirement=room_retirement, retirement=results, normalRemount=True, nativeDocumentPreserved=True)
    finally:
        try:
            main.evaluate("location.hash='/SceneBar';true")
            wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'discovery probe unmounted')
            main.evaluate('window.__joinLifetime?.restore();true')
        finally:
            main.socket.close()
