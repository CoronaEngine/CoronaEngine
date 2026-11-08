"""Exercise named project creation without opening or retaining the created worlds."""
import configparser
import json
import os
from pathlib import Path
import shutil
from urllib.request import urlopen

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


def run(port, root, runtime):
    runtime = Path(runtime).resolve()
    data = (runtime / 'CabbageEditor/data').resolve()
    ini = runtime / 'CoronaEditor.ini'
    original_ini = ini.read_bytes() if ini.exists() else None
    owned_ini = original_ini
    initial = {path.resolve() for path in data.iterdir()} if data.exists() else set()
    created = []
    with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
        targets = [target for target in json.load(response) if target['type'] == 'page'
                   and '/CabbageEditor/Frontend/' in target['url'] and 'uiOwner=' not in target['url']]
    assert len(targets) == 1, targets
    main = Frontend(port, target_id=targets[0]['id'])

    def settings(content):
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.read_string(content.decode('utf-8-sig') if content else '')
        return {(section.casefold(), key.casefold()): value
                for section in config for key, value in config.items(section)}

    try:
        archive = main.evaluate('corona.scene.readActive()')['archive_session']
        before_invalid = {path.resolve() for path in data.iterdir()} if data.exists() else set()
        for method, request in [('createWorldProject', {'mode': 'story'}),
                                ('createWorldProject', {'prompt': 12}),
                                ('createMultiplayerProject', {'role': 'other'}),
                                ('createMultiplayerProject', {'role': 'host', 'path': str(root)})]:
            code = main.evaluate(f'corona.project.{method}({json.dumps(request)}).catch(e=>e.code)')
            assert code == 'INVALID_ARGUMENT', (method, request, code)
        assert ({path.resolve() for path in data.iterdir()} if data.exists() else set()) == before_invalid
        for method, request, prefix in [('createWorldProject', {'prompt': '测试星空世界'}, 'creative_world_'),
                                        ('createMultiplayerProject', {'role': 'host'}, 'multiplayer_host_'),
                                        ('createMultiplayerProject', {'role': 'guest'}, 'multiplayer_guest_')]:
            result = main.evaluate(f'corona.project.{method}({json.dumps(request, ensure_ascii=False)})')
            # Restore only the settings bytes produced by these test-owned calls.
            owned_ini = ini.read_bytes() if ini.exists() else None
            path = Path(result['path']).resolve()
            assert path.parent == data and path not in initial and path.name.startswith(prefix), result
            created.append(path)
            assert set(result) == ({'name', 'path'} if method == 'createWorldProject' else {'name', 'path', 'role'}), result
            config = configparser.ConfigParser(interpolation=None)
            config.read(path / 'scene.ini', encoding='utf-8')
            assert config['scene']['name'] == result['name']
            if method == 'createWorldProject':
                assert config['world']['type'] == 'creative'
                assert config['world']['prompt'] == request['prompt']
            else:
                assert result['role'] == request['role'] == config['multiplayer']['role']
            assert (path / 'assets.manifest.json').is_file()
            assert main.evaluate('corona.scene.readActive()')['archive_session'] == archive
        # Exercise the real creation view without creating an additional world.
        # Hold only the Web facade's delivery of an actual native readiness read.
        main.evaluate("""(async()=>{
          const m=await import(document.querySelector('script[type=module]').src);
          const api=Object.values(m).find(v=>typeof v?.project?.getProjectLoadStatus==='function');
          const original={read:api.project.getProjectLoadStatus,interval:window.setInterval,clear:window.clearInterval};
          const p=window.__creationLifetime={reads:0,pending:[],intervals:new Set(),hold:true};
          api.project.getProjectLoadStatus=(...args)=>{
            p.reads++;const result=original.read(...args);
            return p.hold?Promise.resolve(result).then(value=>new Promise(resolve=>p.pending.push(()=>resolve(value)))):result;
          };
          window.setInterval=(fn,ms,...args)=>{const id=original.interval.call(window,fn,ms,...args);
            if(ms===250&&location.hash.startsWith('#/NewGame'))p.intervals.add(id);return id;};
          window.clearInterval=id=>{p.intervals.delete(id);return original.clear.call(window,id);};
          p.release=()=>{p.hold=false;for(const resume of p.pending.splice(0))resume();};
          p.restore=()=>{p.release();api.project.getProjectLoadStatus=original.read;
            window.setInterval=original.interval;window.clearInterval=original.clear;};
          location.hash='/NewGame';return true;
        })()""")
        wait(lambda: main.evaluate('window.__creationLifetime.pending.length'), lambda n: n > 0,
             'NewGame real native readiness result waiting')
        assert main.evaluate("!!document.querySelector('textarea')")
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('textarea')"),
             bool, 'actual NewGame unmounted')
        retired = main.evaluate('({reads:window.__creationLifetime.reads,intervals:window.__creationLifetime.intervals.size})')
        assert retired['intervals'] == 0, retired
        main.evaluate('window.__creationLifetime.release();new Promise(resolve=>setTimeout(()=>resolve(true),300))')
        assert main.evaluate('window.__creationLifetime.reads') == retired['reads'], 'retired creation polling restarted'
        main.evaluate("location.hash='/NewGame';true")
        wait(lambda: main.evaluate("[...document.querySelectorAll('button')].some(b=>b.textContent.trim()==='创造世界'&&!b.disabled)"),
             bool, 'normal creation remount readiness')
        main.evaluate("location.hash='/SceneBar';true")
        wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')&&!document.querySelector('textarea')"),
             bool, 'creation remount retired')
        assert main.evaluate('window.__creationLifetime.intervals.size') == 0
        return dict(created_worlds=3, roles=['host', 'guest'], invalid_requests_rejected=4,
                    archive_unchanged=True, direct_values=True,
                    creationLifetime=dict(nativeReadinessRetired=True, intervalsReleased=True, normalRemount=True))
    finally:
        retirement_error = None
        try:
            main.evaluate("location.hash='/SceneBar';true")
            wait(lambda: main.evaluate("!!document.querySelector('.scene-tools-panel')"), bool, 'creation probe retired')
            main.evaluate('window.__creationLifetime?.restore();true')
        except Exception as error:
            retirement_error = error
        finally:
            main.socket.close()
        for path in created:
            assert path.parent == data and path not in initial, path
            shutil.rmtree(path)
        current_ini = ini.read_bytes() if ini.exists() else None
        assert current_ini == owned_ini, 'Settings changed outside the test-owned creation calls; preserving newer settings'
        before, after = settings(original_ini), settings(owned_ini)
        assert {key for key in before.keys() | after.keys() if before.get(key) != after.get(key)} <= {('general', 'default_path')}
        if original_ini is not None:
            temporary = ini.with_name(ini.name + '.creation-smoke.tmp')
            # Exclusive creation avoids replacing an unrelated file on interrupted runs.
            with temporary.open('xb') as output:
                output.write(original_ini)
            os.replace(temporary, ini)
        elif ini.exists():
            ini.unlink()
        if retirement_error is not None:
            raise retirement_error
