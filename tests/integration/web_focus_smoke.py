"""Verify deployed SceneBar focus applies once and updates the main viewport UI."""
import json
from pathlib import Path
import sys
import time
from urllib.request import urlopen
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from vision_frontend_probe import Frontend
from web_environment_smoke import wait
from web_view_lifetime_smoke import run_camera
from web_panel_lifecycle_smoke import minimize_restore


def run(port, root, engine_pid):
    def targets():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=20) as response:
            return [target for target in json.load(response) if target['type'] == 'page'
                    and '/CabbageEditor/Frontend/' in target['url']]

    main_targets = [target for target in targets() if 'uiOwner=' not in target['url'] and 'standalone=1' not in target['url']]
    assert len(main_targets) == 1, main_targets
    main = Frontend(port, target_id=main_targets[0]['id'])
    panel = None
    camera_view = None
    tab_id = None
    try:
        main.evaluate("localStorage.setItem('corona.nativeViewportCursor.enabled','1');localStorage.setItem('corona.activeProjectPath',"
                      + json.dumps(str(root)) + ");location.hash='/'")
        wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'MainPage mount')
        # The DOM surface precedes asynchronous camera/render setup. Exercise
        # input only once the mounted hook has installed the viewport lifecycle.
        wait(lambda: main.call('Runtime.evaluate', {
            'expression': "Boolean(getEventListeners(document).keydown?.some(e=>e.listener.toString().includes('new-tab-name')) && getEventListeners(window).blur?.length)",
            'includeCommandLineAPI': True, 'returnByValue': True,
        })['result'].get('value'), bool, 'MainPage input lifecycle ready')
        scope = main.evaluate('corona.scene.readActive()')['archive_session']

        def cameras():
            return main.evaluate('corona.scene.readScene({scene:"scene.ini",cameras:true,...' + json.dumps({'archive_session': scope}) + '})')['cameras']

        initial = cameras()[0]
        service = """(async()=>{const m=await import(document.querySelector('script[type=module]').src);
          return Object.values(m).find(v=>typeof v?.openNativePanel==='function');})()"""
        wait(lambda: main.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda value: value and value.get('enabled'), 'Web owner ready')
        opened = main.evaluate(f"{service}.then(s=>s.openNativePanel({{detached:true,panelId:'SceneTools',routePath:'/SceneBar',width:700,height:800}}))")
        tab_id = opened['tab_id']
        target = wait(lambda: next((target for target in targets() if '#/SceneBar?' in target['url']
                                   and 'standalone=1' in target['url']), None), bool, 'SceneTools document')
        panel = Frontend(port, target_id=target['id'])
        wait(lambda: panel.evaluate("!!document.querySelector('[data-actor-name=Probe]')"), bool, 'SceneBar Actor row')
        monitor = """(async()=>{if(typeof corona.editor!=='undefined')throw Error('Generic native dispatch remains');
          const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.scene?.updateCamera==='function');
          if(!api)throw Error('Shared camera properties facade missing');
          if(typeof api.scene.getSnapshot!=='undefined'||typeof api.sceneTools?.listCameraViews!=='undefined'||typeof api.sceneTools?.listSceneTree!=='undefined')
            throw Error('Retired scene read facade exists');
          window.__cameraReads=[];window.__treeReads=[];const readScene=api.scene.readScene;
          api.scene.readScene=(request)=>readScene(request).then(result=>{
            if(request.cameras)window.__cameraReads.push({args:[request],result});
            if(request.actors==='tree')window.__treeReads.push({args:[request],result});return result;});
          if(typeof globalThis.coronaBridge?.cameraMove!=='undefined')throw Error('Retired camera Fast route exists');
          for(const method of ['setCameraViewport','setViewportUiMode','setViewportUiCalibration','viewportUiPointer','setViewportSystemCursorHidden']){
            if(typeof globalThis.coronaBridge?.[method]!=='undefined')throw Error('Retired viewport Fast route exists: '+method);}
          window.__viewportRequests=[];window.__viewportCalls=[];const configure=api.viewport.configure;
          api.viewport.configure=(...args)=>{const request={args,status:'pending'};window.__viewportRequests.push(request);
            return configure(...args).then(result=>{request.status='resolved';window.__viewportCalls.push({args,result});return result;},error=>{request.status='rejected';request.error=String(error);throw error;});};
          window.__pickCalls=[];const pick=api.viewport.pick;
          api.viewport.pick=(...args)=>pick(...args).then(result=>{
            window.__pickCalls.push({args,result});return result;});
          window.__scriptInputs=[];const dispatch=api.input.dispatch;
          api.input.dispatch=(...args)=>dispatch(...args).then(result=>{
            window.__scriptInputs.push({args,result});return result;});
          window.__cameraMoveResults=[];window.__cameraMoveCalls=[];const move=api.scene.updateCamera;
          api.scene.updateCamera=(...args)=>{window.__cameraMoveCalls.push(args);return move(...args).then(result=>{window.__cameraMoveResults.push({result});return result;},error=>{window.__cameraMoveResults.push({error:String(error),code:error.code});throw error;});};return true;})()"""
        main.evaluate(monitor)
        panel.evaluate(monitor)
        time.sleep(.4)
        main.evaluate('window.__cameraMoveCalls=[];true')
        panel.evaluate("document.querySelector('[data-actor-name=Probe]').dispatchEvent(new MouseEvent('dblclick',{bubbles:true,detail:2}));true")
        focused = wait(lambda: cameras()[0], lambda camera: camera['position'] != initial['position'], 'native focus applied')
        time.sleep(.3)
        assert main.evaluate('window.__cameraMoveCalls') == [], 'Focus must not submit a second camera write'
        assert focused['fov'] == initial['fov']
        assert focused['camera_id'] == initial['camera_id']
        main.evaluate("document.querySelector('[data-viewport-pick-surface]').dispatchEvent(new WheelEvent('wheel',{bubbles:true,cancelable:true,deltaY:-100}));true")
        moved = wait(lambda: cameras()[0], lambda camera: camera['position'] != focused['position'], 'movement from applied focus pose')
        assert abs(moved['position'][0] - focused['position'][0]) < 1e-5, (focused, moved)
        assert abs(moved['position'][1] - focused['position'][1]) < 1e-5, (focused, moved)
        assert 0 < moved['position'][2] - focused['position'][2] <= 2, (focused, moved)
        assert len(main.evaluate('window.__cameraMoveCalls')) == 1
        assert main.evaluate('typeof corona.editor') == 'undefined'
        assert panel.evaluate('typeof corona.editor') == 'undefined'
        assert main.evaluate('typeof globalThis.coronaBridge?.computeActorFocusPose') == 'undefined'
        assert panel.evaluate('typeof globalThis.coronaBridge?.computeActorFocusPose') == 'undefined'
        secondary = next(camera for camera in cameras() if camera['camera_id'] == 'focus-camera')
        panel.evaluate("window.__cameraReads=[];(()=>{const row=[...document.querySelectorAll('span[title]')].find(item=>item.title==="
                       + json.dumps(secondary['name']) + ");if(!row)throw Error('Camera row missing');"
                       "row.parentElement.dispatchEvent(new MouseEvent('mouseenter'));})()")
        hover_reads = wait(lambda: panel.evaluate('window.__cameraReads'), bool, 'SceneBar scoped camera hover read')
        assert hover_reads[0]['args'] == [{'scene': 'scene.ini', 'archive_session': scope, 'cameras': True}], hover_reads
        assert hover_reads[0]['result']['archive_session'] == scope, hover_reads
        assert hover_reads[0]['result']['scene'] == 'scene.ini', hover_reads
        # The production camera row owns the only window-open operation. Do not
        # prime view_open through a retired sceneTools toggle before clicking it.
        panel.evaluate("(()=>{const row=[...document.querySelectorAll('span[title]')].find(item=>item.title==="
                       + json.dumps(secondary['name']) + ");if(!row)throw Error('Camera row missing');"
                       "row.parentElement.dispatchEvent(new MouseEvent('dblclick',{bubbles:true,detail:2}));})()")
        camera_target = wait(lambda: next((target for target in targets() if '#/CameraView?' in target['url']
                                         and 'camera=focus-camera' in target['url']), None), bool, 'CameraView document')
        assert parse_qs(urlsplit(camera_target['url']).fragment.split('?', 1)[1])['archive'] == [scope], camera_target
        camera_view = Frontend(port, target_id=camera_target['id'])
        camera_view.call('Runtime.enable')
        wait(lambda: camera_view.evaluate("!!document.querySelector('.input-layer')"), bool, 'CameraView mount')
        # The root is rendered before async camera setup finishes. In this
        # isolated CameraView document its movement listener is installed only
        # after camera load, selection subscription and initial size sync.
        wait(lambda: camera_view.call('Runtime.evaluate', {
            'expression': "Boolean(getEventListeners(window).keydown?.some(e=>e.listener.toString().includes('KeyW')) && getEventListeners(window).blur?.length)",
            'includeCommandLineAPI': True, 'returnByValue': True,
        })['result'].get('value'), bool, 'CameraView input lifecycle ready')
        assert next(camera for camera in cameras() if camera['camera_id'] == 'focus-camera')['view_open'] is True
        tree_reads = wait(lambda: panel.evaluate('window.__treeReads'), bool, 'SceneBar scoped tree refresh after camera open')
        assert tree_reads[-1]['args'] == [{'scene': 'scene.ini', 'archive_session': scope, 'actors': 'tree', 'cameras': True}], tree_reads
        assert tree_reads[-1]['result']['archive_session'] == scope, tree_reads
        assert tree_reads[-1]['result']['scene'] == 'scene.ini', tree_reads
        assert any(actor['name'] == 'Probe' and isinstance(actor['opaque_handle'], str)
                   for actor in tree_reads[-1]['result']['actors']), tree_reads
        camera_view.evaluate(monitor)
        # Select the displayed secondary camera using its actual production row.
        panel.evaluate("(()=>{const row=[...document.querySelectorAll('span[title]')].find(item=>item.title==="
                       + json.dumps(secondary['name']) + ");if(!row)throw Error('Camera row missing');row.parentElement.click();})()")
        main_position = cameras()[0]['position']
        time.sleep(.4)
        camera_view.evaluate('window.__cameraMoveCalls=[];true')
        main.evaluate('window.__cameraMoveCalls=[];true')
        panel.evaluate("document.querySelector('[data-actor-name=Probe]').dispatchEvent(new MouseEvent('dblclick',{bubbles:true,detail:2}));true")
        focused_secondary = wait(lambda: next(camera for camera in cameras() if camera['camera_id'] == 'focus-camera'),
                                 lambda camera: camera['position'] != secondary['position'], 'secondary native focus')
        time.sleep(.3)
        assert cameras()[0]['position'] == main_position, 'Secondary focus must not rebind or move the main viewport'
        assert main.evaluate('window.__cameraMoveCalls') == []
        assert camera_view.evaluate('window.__cameraMoveCalls') == []
        camera_view.evaluate("window.dispatchEvent(new KeyboardEvent('keydown',{bubbles:true,code:'KeyW',key:'w'}));true")
        try:
            wait(lambda: camera_view.evaluate('window.__cameraMoveCalls.filter(call=>Array.isArray(call[2]?.position)).length'), lambda count: count > 0, 'CameraView movement')
        finally:
            camera_view.evaluate("window.dispatchEvent(new KeyboardEvent('keyup',{bubbles:true,code:'KeyW',key:'w'}));true")
        moved_secondary = wait(
            lambda: next(camera for camera in cameras() if camera['camera_id'] == 'focus-camera'),
            lambda camera: camera['position'] != focused_secondary['position'],
            'CameraView native movement from applied focus pose')
        movement_calls = camera_view.evaluate('window.__cameraMoveCalls.filter(call=>Array.isArray(call[2]?.position))')
        assert movement_calls, 'CameraView must submit movement'
        assert movement_calls[0][0:2] == ['scene.ini', 'focus-camera'], movement_calls
        assert movement_calls[0][2]['archive_session'] == scope, movement_calls
        first_position = movement_calls[0][2]['position']
        assert abs(first_position[0] - focused_secondary['position'][0]) < 1e-5, movement_calls
        assert abs(first_position[1] - focused_secondary['position'][1]) < 1e-5, movement_calls
        assert 0 < first_position[2] - focused_secondary['position'][2] < 2, movement_calls
        assert abs(moved_secondary['position'][0] - focused_secondary['position'][0]) < 1e-5
        assert abs(moved_secondary['position'][1] - focused_secondary['position'][1]) < 1e-5
        assert 0 < moved_secondary['position'][2] - focused_secondary['position'][2] < 2, (focused_secondary, moved_secondary)
        assert main.evaluate('window.__cameraMoveCalls') == []
        for client in [main, panel, camera_view]:
            assert client.evaluate('typeof corona.editor') == 'undefined'
        # Click the actual CameraView controls, then inspect the result of the
        # production facade rather than adding hooks to read-only V8 bindings.
        camera_view.evaluate("[...document.querySelectorAll('.ui-mode-switch button')].find(b=>b.textContent.trim()==='3D UI').click();true")
        viewport_calls = wait(lambda: camera_view.evaluate('window.__viewportCalls'),
                              lambda calls: any(call['args'][2].get('mode') == 'stereo3d' for call in calls)
                              and any('calibration' in call['args'][2] for call in calls), 'CameraView mode and calibration')
        for call in viewport_calls:
            assert call['args'][0:2] == ['scene.ini', 'focus-camera'], call
            assert call['args'][2]['archive_session'] == scope, call
            assert call['result']['camera_id'] == 'focus-camera', call
            assert call['result']['archive_session'] == scope, call
        assert viewport_calls[-1]['result']['mode'] == 'stereo3d', viewport_calls
        def move_native_pointer():
            camera_view.evaluate("(()=>{const el=document.querySelector('.input-layer'),r=el.getBoundingClientRect();"
                                 "el.dispatchEvent(new PointerEvent('pointermove',{bubbles:true,pointerId:9,clientX:r.left+r.width/2,clientY:r.top+r.height/2}));return true;})()")
            return wait(lambda: camera_view.evaluate('window.__viewportCalls'),
                        lambda calls: calls[-1]['args'][2].get('pointer', {}).get('cursor') == 'arrow'
                        and calls[-1]['result']['systemCursorHidden'], 'CameraView typed pointer and OS cursor')
        pointer_calls = move_native_pointer()
        assert pointer_calls[-1]['args'][0:2] == ['scene.ini', 'focus-camera'], pointer_calls[-1]
        assert pointer_calls[-1]['args'][2]['archive_session'] == scope, pointer_calls[-1]
        for cancel in ["window.dispatchEvent(new Event('blur'))",
                       "document.querySelector('.input-layer').dispatchEvent(new PointerEvent('pointercancel',{bubbles:true,pointerId:9}))"]:
            print('CameraView cancel check: ' + cancel, flush=True)
            camera_view.evaluate(cancel + ';true')
            wait(lambda: camera_view.evaluate('window.__viewportCalls'),
                 lambda calls: calls[-1]['args'][2].get('pointer', {}).get('cursor') == 'hidden'
                 and not calls[-1]['result']['systemCursorHidden'], 'CameraView cursor restored on blur/cancel')
            move_native_pointer()
        camera_view.evaluate("[...document.querySelectorAll('.ui-mode-switch button')].find(b=>b.textContent.trim()==='2D UI').click();true")
        wait(lambda: camera_view.evaluate('window.__viewportCalls'),
             lambda calls: calls[-1]['result']['mode'] == 'flat2d', 'CameraView returns to 2D')
        # Exercise actual production click handlers, including Promise delivery into
        # selection, after the movement and pointer regressions above have finished.
        tree = main.evaluate('corona.scene.readScene({scene:"scene.ini",actors:"tree",cameras:true,...' + json.dumps({'archive_session': scope}) + '})')
        probe_actor = next(actor for actor in tree['actors'] if actor['name'] == 'Probe')
        main.evaluate("corona.scene.setActorTransform('scene.ini'," + json.dumps(probe_actor['actor_guid'])
                      + "," + json.dumps({'archive_session': scope, 'position': [0, 0, 0],
                                          'rotation': [0, 0, 0], 'scale': [1, 1, 1]}) + ")")
        for document, camera_id, selector, source in [
            (main, initial['camera_id'], '[data-viewport-pick-surface]', 'main'),
            (camera_view, 'focus-camera', '.input-layer', 'cameraView'),
        ]:
            document.evaluate("corona.scene.updateCamera('scene.ini'," + json.dumps(camera_id)
                              + "," + json.dumps({'archive_session': scope, 'position': [-.25, -.25, 3],
                                                  'forward': [0, 0, -1], 'world_up': [0, 1, 0], 'fov': 60}) + ")")
            # CameraView renders behind the title bar as well; its input layer
            # excludes that bar. Aim at the render center, inside the hit layer.
            point = document.evaluate("(()=>{const el=document.querySelector(" + json.dumps(selector)
                                      + "),r=el.getBoundingClientRect();return {x:"
                                      + ("innerWidth/2,y:innerHeight/2" if source == 'cameraView'
                                         else "r.left+r.width/2,y:r.top+r.height/2") + "};})()")
            # Property acceptance precedes the next GPU frame. Establish the
            # known fixture hit with read-only picks before exercising one DOM
            # click, rather than retrying the production event on stale frames.
            render = document.evaluate("({width:innerWidth,height:innerHeight})" if source == 'cameraView'
                                       else "(()=>{const r=document.querySelector('[data-viewport-pick-surface]').getBoundingClientRect();return {width:r.width,height:r.height,x:r.left,y:r.top};})()")
            pick_options = {'archive_session': scope, 'x': int(point['x'] - render.get('x', 0)),
                            'y': int(point['y'] - render.get('y', 0)),
                            'width': int(render['width']), 'height': int(render['height'])}
            print(f'{source} GPU fixture ready: waiting for first rendered hit', flush=True)
            wait(lambda: document.evaluate("corona.viewport.pick('scene.ini'," + json.dumps(camera_id)
                                           + "," + json.dumps(pick_options)
                                           + ").catch(error=>{if(error.message==='Viewport pick failed: timeout')"
                                             "return {status:'pending-frame'};throw error;})"),
                 lambda value: value.get('actor_guid') == probe_actor['actor_guid'],
                 f'{source} GPU fixture ready', seconds=60)
            document.evaluate("window.__pickCalls=[];window.__scriptInputs=[];true")
            document.evaluate("(()=>{const el=document.querySelector(" + json.dumps(selector)
                              + "),r=el.getBoundingClientRect();el.dispatchEvent(new MouseEvent('click',"
                                "{bubbles:true,detail:1,clientX:" + json.dumps(point['x'])
                              + ",clientY:" + json.dumps(point['y']) + "}));return true;})()")
            picked = wait(lambda: document.evaluate('window.__pickCalls'), bool, f'{source} production click pick Promise')
            assert len(picked) == 1, picked
            assert picked[0]['args'][0:2] == ['scene.ini', camera_id], picked
            assert picked[0]['args'][2]['archive_session'] == scope, picked
            assert picked[0]['result']['status'] == 'success', picked
            assert picked[0]['result']['actor_guid'] == probe_actor['actor_guid'], picked
            assert isinstance(picked[0]['result']['actor_handle'], str), picked
            wait(lambda: main.evaluate('corona.scene.readActive()')['selection'],
                 lambda selection: selection.get('actor_guid') == probe_actor['actor_guid']
                 and selection.get('source_viewport') == source, f'{source} picked Actor selection')
            assert document.evaluate('typeof globalThis.coronaBridge?.pickActor') == 'undefined'
            inputs = wait(lambda: document.evaluate('window.__scriptInputs'), bool, f'{source} scoped script click')
            clicks = [call for call in inputs if call['args'][2].get('event') == 'click']
            assert len(clicks) == 1, inputs
            assert clicks[0]['args'][0:2] == ['scene.ini', camera_id], clicks
            assert clicks[0]['args'][2]['archive_session'] == scope, clicks
            assert clicks[0]['args'][2]['picked_actor'] == probe_actor['name'], clicks
            assert clicks[0]['args'][2]['viewport_width'] > 0 and clicks[0]['result'] == {'status': 'queued'}, clicks
            document.evaluate("window.__scriptInputs=[];window.dispatchEvent(new KeyboardEvent('keydown',{key:'d',code:'KeyD'}));window.dispatchEvent(new KeyboardEvent('keyup',{key:'d',code:'KeyD'}));true")
            assert not any(call['args'][2]['type'].startswith('key') for call in document.evaluate('window.__scriptInputs'))
            assert document.evaluate('typeof globalThis.coronaBridge?.injectInput') == 'undefined'
        def listeners(client):
            return client.call('Runtime.evaluate', {
                'expression': "[document,window].map(target=>Object.fromEntries(['keydown','keyup','resize','blur','visibilitychange'].map(kind=>[kind,(getEventListeners(target)[kind]||[]).map(e=>e.listener.toString()).sort()])))",
                'includeCommandLineAPI': True, 'returnByValue': True,
            })['result']['value']
        before = [listeners(client) for client in (main, camera_view)]
        hidden_restore = minimize_restore(engine_pid, main, camera_view, targets, service)
        assert [listeners(client) for client in (main, camera_view)] == before, 'hide/restore duplicated or lost production listeners'
        camera_view.evaluate('window.__cameraMoveCalls=[];true')
        camera_view.evaluate("window.dispatchEvent(new KeyboardEvent('keydown',{bubbles:true,code:'KeyW',key:'w'}));true")
        try:
            restored_movement = wait(lambda: camera_view.evaluate('window.__cameraMoveCalls.filter(call=>Array.isArray(call[2]?.position))'),
                                     bool, 'CameraView input restored after real HWND minimization')
        finally:
            camera_view.evaluate("window.dispatchEvent(new KeyboardEvent('keyup',{bubbles:true,code:'KeyW',key:'w'}));true")
        assert all(call[1] == 'focus-camera' and call[2]['archive_session'] == scope for call in restored_movement), restored_movement
        hidden_restore.update(listeners_preserved=True, camera_input_restored=True)
        camera_lifetime = run_camera(camera_view)
        return {'cameraHiddenRestore': hidden_restore, 'cameraLifetime': camera_lifetime, 'documents': 3, 'archiveSession': scope, 'cameraId': focused['camera_id'],
                'focusPosition': focused['position'], 'movementFromFocusPosition': moved['position'],
                'focusResubmissions': 0, 'movementSubmissions': 1, 'retiredFocusRequests': 0,
                'cameraHoverRead': 'scoped typed V8 projection', 'cameraViewArchiveInRoute': True,
                'retiredSceneReadRequests': 0,
                'sceneTreeRead': 'actual SceneBar scoped V8 refresh preserves Actor rows and camera windows',
                'secondaryFocusPosition': focused_secondary['position'], 'secondaryMovementPosition': moved_secondary['position'],
                'secondaryFocusDoesNotRebindMain': True, 'viewportModeAndCalibration': 'shared scoped V8 operation',
                'retiredViewportBindings': 0, 'viewportPointer': 'typed scoped state, actual DOM movement, blur/cancel restore OS cursor',
                'viewportPick': 'actual main and CameraView DOM clicks, one V8 Promise each, GPU hit and source-scoped selection',
                'scriptInput': 'actual main and CameraView click uses one scoped typed input with picked Actor; DOM keyboard does not duplicate SDL'}
    except Exception as error:
        diagnostic = {}
        for name, client in [('main', main), ('panel', panel), ('camera', camera_view)]:
            if client is None:
                continue
            try:
                diagnostic[name] = client.evaluate("({url:location.href,text:document.body.innerText.slice(0,2000),moves:window.__cameraMoveCalls,results:window.__cameraMoveResults,viewportRequests:window.__viewportRequests})")
            except Exception as failure:
                diagnostic[name] = str(failure)
        if camera_view:
            diagnostic['camera_events'] = [event for event in camera_view.events if event.get('method') in ('Runtime.exceptionThrown', 'Runtime.consoleAPICalled')]
        print('Focus failure: ' + repr(error), flush=True)
        print('Focus failure diagnostics: ' + json.dumps(diagnostic, ensure_ascii=False), flush=True)
        raise
    finally:
        # Preserve the first assertion when native cleanup fails as well. The
        # owning runner still retires this launch's complete process tree.
        failed = sys.exc_info()[0] is not None
        cleanup_errors = []
        for expression in (["corona.window.closeCameraView('scene.ini','focus-camera')"] if camera_view else []) + (
                [f'corona.window.closeTab({tab_id})'] if tab_id is not None else []):
            try:
                main.evaluate(expression)
            except Exception as error:
                cleanup_errors.append(str(error))
        for client in [camera_view, panel, main]:
            if client:
                client.socket.close()
        if cleanup_errors:
            if failed:
                print('Focus cleanup failures: ' + repr(cleanup_errors), flush=True)
            else:
                raise RuntimeError('Focus cleanup failures: ' + repr(cleanup_errors))
