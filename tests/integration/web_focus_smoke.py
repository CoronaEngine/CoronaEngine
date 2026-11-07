"""Verify deployed SceneBar focus applies once and updates the main viewport UI."""
import json
from pathlib import Path
import sys
import time
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def run(port, root):
    def targets():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
            return [target for target in json.load(response) if target['type'] == 'page'
                    and '/CabbageEditor/Frontend/' in target['url']]

    main = Frontend(port, target_id=targets()[0]['id'])
    panel = None
    camera_view = None
    tab_id = None
    try:
        main.evaluate("localStorage.setItem('corona.nativeViewportCursor.enabled','1');localStorage.setItem('corona.activeProjectPath',"
                      + json.dumps(str(root)) + ");location.hash='/'")
        wait(lambda: main.evaluate("!!document.querySelector('[data-viewport-pick-surface]')"), bool, 'MainPage mount')
        scope = main.evaluate('corona.scene.readActive()')['archive_session']

        def cameras():
            result = main.api('SceneTools.list_camera_views', ['scene.ini'])
            assert result['success'], result
            return result['data']['cameras']

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
        monitor = """(async()=>{window.__oldFocusRequests=[];const invoke=corona.editor.invoke;
          corona.editor.invoke=(api,args)=>{if(['SceneTools.focus_actor','ResourceSearch.focus_actor',
            'SceneTools.set_camera_pose','SceneTools.update_camera_view'].includes(api)){
            window.__oldFocusRequests.push(api);throw Error('Retired focus route');}return invoke(api,args);};
          const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.scene?.setCameraProperties==='function');
          if(!api)throw Error('Shared camera properties facade missing');
          if(typeof coronaBridge.cameraMove!=='undefined')throw Error('Retired camera Fast route exists');
          for(const method of ['setCameraViewport','setViewportUiMode','setViewportUiCalibration','viewportUiPointer','setViewportSystemCursorHidden']){
            if(typeof coronaBridge[method]!=='undefined')throw Error('Retired viewport Fast route exists: '+method);}
          window.__viewportCalls=[];const configure=api.viewport.configure;
          api.viewport.configure=(...args)=>configure(...args).then(result=>{
            window.__viewportCalls.push({args,result});return result;});
          window.__pickCalls=[];const pick=api.viewport.pick;
          api.viewport.pick=(...args)=>pick(...args).then(result=>{
            window.__pickCalls.push({args,result});return result;});
          window.__scriptInputs=[];const dispatch=api.input.dispatch;
          api.input.dispatch=(...args)=>dispatch(...args).then(result=>{
            window.__scriptInputs.push({args,result});return result;});
          window.__cameraMoveCalls=[];const move=api.scene.setCameraProperties;
          api.scene.setCameraProperties=(...args)=>{window.__cameraMoveCalls.push(args);return move(...args);};return true;})()"""
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
        assert main.evaluate('window.__oldFocusRequests') == []
        assert panel.evaluate('window.__oldFocusRequests') == []
        assert main.evaluate('typeof coronaBridge.computeActorFocusPose') == 'undefined'
        assert panel.evaluate('typeof coronaBridge.computeActorFocusPose') == 'undefined'
        secondary = next(camera for camera in cameras() if camera['camera_id'] == 'focus-camera')
        opened_camera = main.api('SceneTools.open_camera_view', ['scene.ini', secondary['camera_id']])
        assert opened_camera['success'], opened_camera
        camera_snapshot = {**opened_camera['data']['camera'], 'scene_id': 'scene.ini'}
        main.evaluate(f'{service}.then(s=>s.createCameraView({json.dumps(camera_snapshot)}))')
        camera_target = wait(lambda: next((target for target in targets() if '#/CameraView?' in target['url']
                                         and 'camera=focus-camera' in target['url']), None), bool, 'CameraView document')
        camera_view = Frontend(port, target_id=camera_target['id'])
        wait(lambda: camera_view.evaluate("!!document.querySelector('.input-layer')"), bool, 'CameraView mount')
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
            wait(lambda: camera_view.evaluate('window.__cameraMoveCalls.length'), lambda count: count > 0, 'CameraView movement')
        finally:
            camera_view.evaluate("window.dispatchEvent(new KeyboardEvent('keyup',{bubbles:true,code:'KeyW',key:'w'}));true")
        moved_secondary = wait(
            lambda: next(camera for camera in cameras() if camera['camera_id'] == 'focus-camera'),
            lambda camera: camera['position'] != focused_secondary['position'],
            'CameraView native movement from applied focus pose')
        movement_calls = camera_view.evaluate('window.__cameraMoveCalls')
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
            assert client.evaluate('window.__oldFocusRequests') == []
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
        tree = main.api('SceneTools.list_scene_tree', ['scene.ini'])['data']
        probe_actor = next(actor for actor in tree['actors'] if actor['name'] == 'Probe')
        main.evaluate("corona.scene.setActorTransform('scene.ini'," + json.dumps(probe_actor['actor_guid'])
                      + "," + json.dumps({'archive_session': scope, 'position': [0, 0, 0],
                                          'rotation': [0, 0, 0], 'scale': [1, 1, 1]}) + ")")
        for document, camera_id, selector, source in [
            (main, initial['camera_id'], '[data-viewport-pick-surface]', 'main'),
            (camera_view, 'focus-camera', '.input-layer', 'cameraView'),
        ]:
            document.evaluate("corona.scene.setCameraProperties('scene.ini'," + json.dumps(camera_id)
                              + "," + json.dumps({'archive_session': scope, 'position': [-.25, -.25, 3],
                                                  'forward': [0, 0, -1], 'world_up': [0, 1, 0], 'fov': 60}) + ")")
            document.evaluate("window.__pickCalls=[];window.__scriptInputs=[];true")
            document.evaluate("(()=>{const el=document.querySelector(" + json.dumps(selector)
                              + "),r=el.getBoundingClientRect();el.dispatchEvent(new MouseEvent('click',"
                                "{bubbles:true,detail:1,clientX:r.left+r.width/2,clientY:r.top+r.height/2}));return true;})()")
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
            assert document.evaluate('typeof coronaBridge.pickActor') == 'undefined'
            inputs = wait(lambda: document.evaluate('window.__scriptInputs'), bool, f'{source} scoped script click')
            clicks = [call for call in inputs if call['args'][2].get('event') == 'click']
            assert len(clicks) == 1, inputs
            assert clicks[0]['args'][0:2] == ['scene.ini', camera_id], clicks
            assert clicks[0]['args'][2]['archive_session'] == scope, clicks
            assert clicks[0]['args'][2]['picked_actor'] == probe_actor['name'], clicks
            assert clicks[0]['args'][2]['viewport_width'] > 0 and clicks[0]['result'] == {'status': 'queued'}, clicks
            document.evaluate("window.__scriptInputs=[];window.dispatchEvent(new KeyboardEvent('keydown',{key:'d',code:'KeyD'}));window.dispatchEvent(new KeyboardEvent('keyup',{key:'d',code:'KeyD'}));true")
            assert not any(call['args'][2]['type'].startswith('key') for call in document.evaluate('window.__scriptInputs'))
            assert document.evaluate('typeof coronaBridge.injectInput') == 'undefined'
        return {'documents': 3, 'archiveSession': scope, 'cameraId': focused['camera_id'],
                'focusPosition': focused['position'], 'movementFromFocusPosition': moved['position'],
                'focusResubmissions': 0, 'movementSubmissions': 1, 'retiredFocusRequests': 0,
                'secondaryFocusPosition': focused_secondary['position'], 'secondaryMovementPosition': moved_secondary['position'],
                'secondaryFocusDoesNotRebindMain': True, 'viewportModeAndCalibration': 'shared scoped V8 operation',
                'retiredViewportBindings': 0, 'viewportPointer': 'typed scoped state, actual DOM movement, blur/cancel restore OS cursor',
                'viewportPick': 'actual main and CameraView DOM clicks, one V8 Promise each, GPU hit and source-scoped selection',
                'scriptInput': 'actual main and CameraView click uses one scoped typed input with picked Actor; DOM keyboard does not duplicate SDL'}
    finally:
        if camera_view:
            main.evaluate("corona.window.closeCameraView('scene.ini','focus-camera')")
            camera_view.socket.close()
        if tab_id is not None:
            main.evaluate(f'corona.window.closeTab({tab_id})')
        if panel:
            panel.socket.close()
        main.socket.close()
