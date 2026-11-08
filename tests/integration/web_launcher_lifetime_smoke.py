"""Retire real StartScreen animations, including the delayed exit action."""
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
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'launcher baseline route')
        main.evaluate("""(async()=>{
          const m=await import(document.querySelector('script[type=module]').src);
          const service=Object.values(m).find(v=>typeof v?.openNativePanel==='function');
          const original={close:service.closeProcess,raf:window.requestAnimationFrame,cancel:window.cancelAnimationFrame,
            timeout:window.setTimeout,clear:window.clearTimeout,
            addWindow:window.addEventListener,removeWindow:window.removeEventListener,
            addDocument:document.addEventListener,removeDocument:document.removeEventListener};
          const p=window.__launcherLifetime={closed:0,frames:new Set(),timers:new Set(),listeners:new Set(),nodes:[]};
          // Observe the production exit action without terminating the test Job.
          service.closeProcess=()=>{p.closed++;return Promise.resolve();};
          window.requestAnimationFrame=callback=>{
            const owned=String(callback).includes('.getElapsedTime()');
            const id=original.raf.call(window,time=>{p.frames.delete(id);callback(time);});
            if(owned)p.frames.add(id);return id;
          };
          window.cancelAnimationFrame=id=>{p.frames.delete(id);return original.cancel.call(window,id);};
          window.setTimeout=(callback,ms,...args)=>{
            const id=original.timeout.call(window,()=>{p.timers.delete(id);callback(...args);},ms);
            if(ms===80&&location.hash.startsWith('#/StartScreen'))p.timers.add(id);return id;
          };
          window.clearTimeout=id=>{p.timers.delete(id);return original.clear.call(window,id);};
          const owned=(type,fn)=>type==='mousemove'&&String(fn).includes('.clientX')
            ||type==='resize'&&String(fn).includes('.updateProjectionMatrix()');
          window.addEventListener=(type,fn,...args)=>{if(owned(type,fn))p.listeners.add(fn);return original.addWindow.call(window,type,fn,...args);};
          document.addEventListener=(type,fn,...args)=>{if(owned(type,fn))p.listeners.add(fn);return original.addDocument.call(document,type,fn,...args);};
          window.removeEventListener=(type,fn,...args)=>{p.listeners.delete(fn);return original.removeWindow.call(window,type,fn,...args);};
          document.removeEventListener=(type,fn,...args)=>{p.listeners.delete(fn);return original.removeDocument.call(document,type,fn,...args);};
          p.styles=()=>p.nodes.map(node=>node.getAttribute('style'));
          p.read=()=>({closed:p.closed,frames:p.frames.size,timers:p.timers.size,listeners:p.listeners.size});
          p.turn=ms=>new Promise(resolve=>original.timeout.call(window,()=>resolve(true),ms));
          p.restore=()=>{service.closeProcess=original.close;window.requestAnimationFrame=original.raf;window.cancelAnimationFrame=original.cancel;
            window.setTimeout=original.timeout;window.clearTimeout=original.clear;
            window.addEventListener=original.addWindow;window.removeEventListener=original.removeWindow;
            document.addEventListener=original.addDocument;document.removeEventListener=original.removeDocument;};
          return true;
        })()""")
        for action in ('exit', 'panel', 'cancel'):
            main.evaluate("location.hash='/StartScreen';true")
            wait(lambda: main.evaluate("!!document.querySelector('.start-screen-root canvas')"), bool, 'real launcher rendered')
            active = main.evaluate('window.__launcherLifetime.read()')
            assert active['frames'] == 1 and active['listeners'] == 2, active
            main.evaluate(f"document.querySelectorAll('.nav-btn')[{0 if action == 'panel' else 3}].click();true")
            wait(lambda: main.evaluate("!!document.querySelector('.page-panel')"), bool, 'real launcher panel mounted')
            main.evaluate("window.__launcherLifetime.nodes=[document.querySelector('.nav-container'),document.querySelector('.page-panel')];true")
            if action != 'panel':
                main.evaluate('document.querySelector(' + json.dumps('.exit-action.confirm' if action == 'exit' else '.exit-action.cancel') + ').click();true')
            main.evaluate("window.dispatchEvent(new Event('resize'));location.hash='/SceneBar';true")
            wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('.start-screen-root')"),
                 bool, 'actual launcher retired during ' + action)
            retired_styles = main.evaluate('window.__launcherLifetime.styles()')
            main.evaluate('window.__launcherLifetime.turn(900)')
            retired = main.evaluate('window.__launcherLifetime.read()')
            assert retired['closed'] == 0, 'retired launcher attempted process exit: ' + repr(retired)
            assert retired == dict(closed=0, frames=0, timers=0, listeners=0), retired
            assert main.evaluate('window.__launcherLifetime.styles()') == retired_styles, 'retired GSAP tween still mutates detached DOM'
            results.append(dict(action=action, **retired))
        hints = run_hint_lifetime(main)
        return dict(retirement=results, hintLifetime=hints, realVueActions=True, nativeDocumentPreserved=True,
                    exitBoundary='Web closeProcess observed; native process exit deliberately intercepted')
    finally:
        try:
            main.evaluate("location.hash='/SceneBar';true")
            wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'launcher probe retired')
            main.evaluate('window.__launcherLifetime?.restore();true')
        finally:
            main.socket.close()


def run_hint_lifetime(main):
    """Drive Pet's registered hint callback and the actual teleported bubble DOM."""
    try:
        main.evaluate("""(()=>{
          const original={timeout:window.setTimeout,clear:window.clearTimeout,interval:window.setInterval,clearInterval:window.clearInterval};
          const p=window.__hintLifetime={timers:new Map(),intervals:new Map(),held:[],hint:null};
          window.setTimeout=(fn,ms,...args)=>{
            if(!location.hash.startsWith('#/Pet')||![6000,220].includes(ms))return original.timeout.call(window,fn,ms,...args);
            const fire=()=>{p.timers.delete(id);fn(...args);};
            const id=original.timeout.call(window,()=>ms===220?p.held.push(fire):fire(),ms);
            p.timers.set(id,ms);return id;
          };
          window.clearTimeout=id=>{p.timers.delete(id);return original.clear.call(window,id);};
          window.setInterval=(fn,ms,...args)=>{
            const id=original.interval.call(window,fn,ms,...args);
            if(location.hash.startsWith('#/Pet')&&[50,10000].includes(ms)){p.intervals.set(id,ms);if(ms===10000)p.hint=fn;}
            return id;
          };
          window.clearInterval=id=>{p.intervals.delete(id);return original.clearInterval.call(window,id);};
          p.read=()=>({hide:[...p.timers.values()].filter(ms=>ms===6000).length,
            close:[...p.timers.values()].filter(ms=>ms===220).length,
            progress:[...p.intervals.values()].filter(ms=>ms===50).length,
            hint:[...p.intervals.values()].filter(ms=>ms===10000).length});
          p.bubble=()=>[...document.querySelectorAll('.rounded-tl-sm')].map(node=>node.parentElement)
            .find(node=>!node.classList.contains('bubble-pop-leave-active'));
          p.release=()=>{for(const fire of p.held.splice(0))fire();};
          p.turn=ms=>new Promise(resolve=>original.timeout.call(window,()=>resolve(true),ms));
          p.restore=()=>{p.release();for(const id of p.timers.keys())original.clear.call(window,id);
            for(const id of p.intervals.keys())original.clearInterval.call(window,id);
            window.setTimeout=original.timeout;window.clearTimeout=original.clear;
            window.setInterval=original.interval;window.clearInterval=original.clearInterval;};
          location.hash='/Pet';return true;
        })()""")
        wait(lambda: main.evaluate("typeof window.__hintLifetime.hint==='function'"), bool, 'real Pet hint timer registered')
        main.evaluate('window.__hintLifetime.hint()')
        wait(lambda: main.evaluate('!!window.__hintLifetime.bubble()'), bool, 'actual teleported hint visible')
        main.evaluate("window.__hintLifetime.bubble().dispatchEvent(new MouseEvent('mouseenter'));true")
        hover = main.evaluate('window.__hintLifetime.read()')
        assert hover == dict(hide=0, close=0, progress=0, hint=1), 'hover did not pause actual bubble: ' + repr(hover)
        main.evaluate("window.__hintLifetime.bubble().dispatchEvent(new MouseEvent('mouseleave'));true")
        resumed = main.evaluate('window.__hintLifetime.read()')
        assert resumed == dict(hide=1, close=0, progress=1, hint=1), resumed
        main.evaluate("window.__hintLifetime.bubble().querySelector('button').click();true")
        closing = main.evaluate('window.__hintLifetime.read()')
        assert closing == dict(hide=0, close=1, progress=0, hint=1), closing
        main.evaluate('window.__hintLifetime.hint()')
        replacement = wait(lambda: main.evaluate('window.__hintLifetime.read()'),
                           lambda p: p['hide'] == 1 and p['progress'] == 1, 'new hint mounted before previous close')
        assert replacement['close'] == 0, 'old close timer survived its hint component: ' + repr(replacement)
        main.evaluate("window.__hintLifetime.bubble().querySelector('button').click();true")
        assert main.evaluate('window.__hintLifetime.read().close') == 1, 'replacement close must be pending before retirement'
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'hint page actually unmounted')
        retired = main.evaluate('window.__hintLifetime.read()')
        assert retired == dict(hide=0, close=0, progress=0, hint=0), retired
        main.evaluate('window.__hintLifetime.turn(250)')
        main.evaluate('window.__hintLifetime.release();true')
        assert main.evaluate('window.__hintLifetime.read()') == retired
        return dict(hoverPaused=True, mouseleaveResumed=True, replacementRetiredOldClose=True,
                    unmountedTimers=retired, realTeleport=True)
    finally:
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'hint probe retired')
        main.evaluate('window.__hintLifetime?.restore();true')
