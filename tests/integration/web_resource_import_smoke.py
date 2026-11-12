"""Local positive resource/IK acceptance; uses only the test process's file dialog."""
import ctypes
from ctypes import wintypes
import json
import math
from pathlib import Path
import shutil
import struct
import sys
import threading
import wave
import zlib

from vision_frontend_probe import Frontend
from web_environment_smoke import wait


class FileDialog:
    def __init__(self, pid):
        self.pid = pid
        self.user = ctypes.WinDLL('user32', use_last_error=True)
        self.callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        api = self.user
        api.EnumWindows.argtypes = [self.callback, wintypes.LPARAM]
        api.EnumChildWindows.argtypes = [wintypes.HWND, self.callback, wintypes.LPARAM]
        api.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        api.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        api.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        api.GetDlgCtrlID.argtypes = [wintypes.HWND]
        api.GetParent.argtypes = [wintypes.HWND]
        api.GetParent.restype = wintypes.HWND
        api.IsWindow.argtypes = [wintypes.HWND]
        api.IsWindowVisible.argtypes = [wintypes.HWND]
        api.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        api.SendMessageW.restype = ctypes.c_ssize_t
        api.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]

    def label(self, hwnd, method):
        value = ctypes.create_unicode_buffer(512)
        method(hwnd, value, len(value))
        return value.value

    def windows(self):
        found = []
        @self.callback
        def visit(hwnd, _):
            pid = wintypes.DWORD()
            self.user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if (pid.value == self.pid and self.user.IsWindowVisible(hwnd)
                    and self.label(hwnd, self.user.GetWindowTextW) == '选择导入资源'):
                found.append(hwnd)
            return True
        self.user.EnumWindows(visit, 0)
        return found

    def close(self, hwnd, path=None):
        if path is not None:
            edits = []
            @self.callback
            def visit(child, _):
                if (self.user.IsWindowVisible(child) and self.label(child, self.user.GetClassNameW) == 'Edit'
                        and (self.user.GetDlgCtrlID(child) == 0x480
                             or self.label(self.user.GetParent(child), self.user.GetClassNameW) in ('ComboBox', 'ComboBoxEx32'))):
                    edits.append(child)
                return True
            def filename():
                edits.clear()
                self.user.EnumChildWindows(hwnd, visit, 0)
                return edits[0] if len(edits) == 1 else None
            edit = wait(filename, bool, 'resource filename edit')
            value = ctypes.create_unicode_buffer(str(path))
            assert self.user.SendMessageW(edit, 0xC, 0, ctypes.addressof(value))
        assert self.user.PostMessageW(hwnd, 0x111, 1 if path else 2, 0)
        wait(lambda: bool(self.user.IsWindow(hwnd)), lambda exists: not exists, 'resource dialog closes')

    def drive(self, action, path=None):
        errors = []
        def worker():
            try:
                self.close(wait(self.windows, bool, 'resource file dialog')[0], path)
            except Exception as error:
                errors.append(error)
                for hwnd in self.windows():
                    self.close(hwnd)
        thread = threading.Thread(target=worker, daemon=True)
        thread.start()
        try:
            result = action()
        finally:
            thread.join(timeout=35)
        assert not thread.is_alive(), 'Resource dialog worker did not finish'
        if errors:
            raise errors[0]
        return result


def run(port, root, engine_pid, repo):
    web = Frontend(port)
    dialog = FileDialog(engine_pid)
    def v8(method, *args):
        return web.evaluate(method + '(...' + json.dumps(args) + ')')
    try:
        wait(lambda: web.evaluate("!!document.querySelector('#app[data-v-app] button')"), bool, 'editor mounted')
        opened = wait(lambda: v8('corona.project.openProject', str(root)),
                      lambda value: value.get('status') != 'service_initializing', 'open resource world')
        assert opened['ok'], opened
        active = v8('corona.scene.readActive')
        scene, scope = active['scene'], {'archive_session': active['archive_session']}
        web.evaluate("location.hash='/SceneBar'")
        toolbar = "document.querySelector('[data-guidance=scene-import-model]')"
        wait(lambda: web.evaluate(f'!!{toolbar}'), bool, 'SceneBar mounted')
        def actors():
            return v8('corona.scene.readScene', {**scope, 'scene': scene, 'actors': 'index'})['actors']
        def imported_actor(label, source, actor_type):
            before = {a['actor_guid'] for a in actors()}
            web.evaluate(f"{toolbar}.querySelector('button').click()")
            action = lambda: web.evaluate(f"[...{toolbar}.querySelectorAll('button')].find(b=>b.textContent.includes({json.dumps(label)})).click()")
            dialog.drive(action, source)
            # The lightweight index projects source extensions, not semantic
            # Actor categories. Verify the category through the Actor projection.
            extension = source.suffix.lstrip('.').lower()
            result = wait(actors, lambda values: any(a['actor_guid'] not in before and a['type'] == extension for a in values), 'Vue resource import')
            actor = next(a for a in result if a['actor_guid'] not in before and a['type'] == extension)
            properties = v8('corona.scene.readActorProperties', scene, actor['actor_guid'], scope)
            assert properties['actor_type'] == actor_type, properties
            return actor
        before = actors()
        cancelled = dialog.drive(lambda: v8('corona.scene.importResourceFile', scene, {**scope, 'file_type': 'model'}))
        assert cancelled['status'] == 'canceled' and actors() == before, cancelled
        source = root.parent / 'acceptance-assets'
        source.mkdir(exist_ok=True)
        shutil.copytree(repo / 'CoronaEditor/assets/wolf', source / 'WolfSource', dirs_exist_ok=True)
        wolf = source / 'WolfSource/fbx/Wolf.fbx'
        model = imported_actor('模型', wolf, 'model')
        def skeleton():
            value = web.evaluate('corona.scene.readActorRig(...' + json.dumps([scene, model['actor_guid'], scope]) + ').catch(error=>({error:error.code}))')
            value = value.get('skeleton', {})
            return value if value.get('is_skinned') and value.get('bone_count', 0) > 0 else None
        skel = wait(skeleton, bool, 'imported skeleton')
        leaf = next(node for node in skel['nodes'] if node['leaf'] and node['parent'] >= 0)
        before_ik = v8('corona.scene.readActorRig', scene, model['actor_guid'], scope)['ik']
        chain = {'id': 'acceptance-foot', 'bone_name': leaf['name'], 'mode': 'foot_plant',
                 'chain_length': 2, 'weight': 0.5, 'enabled': False}
        changed = v8('corona.scene.setActorIkChains', scene, model['actor_guid'], [chain], before_ik['revision'], scope)
        assert changed['revision'] > before_ik['revision'] and changed['chains'][0]['id'] == chain['id'], changed
        conflict = web.evaluate('corona.scene.setActorIkChains(...' + json.dumps([scene, model['actor_guid'], [], before_ik['revision'], scope]) + ').catch(error=>error.code)')
        assert conflict == 'CONFLICT', conflict
        assert v8('corona.scene.readActorRig', scene, model['actor_guid'], scope)['ik']['revision'] == changed['revision']
        restored = v8('corona.scene.setActorIkChains', scene, model['actor_guid'], before_ik['chains'], changed['revision'], scope)
        assert len(restored['chains']) == len(before_ik['chains']), restored
        image = source / 'AcceptanceImage.png'
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        image.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 6, 0, 0, 0))
                          + chunk(b'IDAT', zlib.compress(b'\x00\xff\x00\x00\xff\x00\xff\x00\xff' * 2)) + chunk(b'IEND', b''))
        ui_image = imported_actor('UI图片', image, 'ui_image')
        tone = source / 'AcceptanceTone.wav'
        with wave.open(str(tone), 'wb') as stream:
            stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            stream.writeframes(b''.join(struct.pack('<h', int(3000 * math.sin(2 * math.pi * 997 * n / 48000))) for n in range(48000)))
        audio = imported_actor('音频', tone, 'audio')
        wait(lambda: next((actor for actor in v8('corona.scene.readScene', {**scope, 'scene': scene, 'actors': 'tree', 'cameras': True})['actors']
                           if actor['actor_guid'] == audio['actor_guid']), {}),
             lambda value: bool(value.get('audio_resource_id')) and value.get('load_status') == 'loaded', 'audio resource loaded')
        # Capture only the output-device loopback in memory, never a microphone;
        # retain scalar measurements, not raw device samples.
        sys.path.insert(0, str(repo / 'build/acceptance-deps'))
        import soundcard
        import numpy as np
        speaker = soundcard.default_speaker()
        assert speaker is not None, 'No default audio output device'
        loopback = soundcard.get_microphone(id=speaker.id, include_loopback=True)
        assert loopback.isloopback, 'Audio probe must use output loopback'
        def amplitude(samples):
            carrier = np.exp(-2j * np.pi * 997 * np.arange(len(samples)) / 48000)
            return float(np.max(np.abs(2 * np.mean(samples * carrier[:, None], axis=0))))
        with loopback.recorder(samplerate=48000) as recorder:
            baseline = amplitude(recorder.record(numframes=24000))
            played = v8('corona.audio.play', {'scene': scene, 'actor': audio['actor_guid'], **scope, 'loop': True})
            try:
                playing = amplitude(recorder.record(numframes=96000))
            finally:
                stopped = v8('corona.audio.stop', {'scene': scene, 'actor': audio['actor_guid'], **scope})
            recorder.record(numframes=48000)  # discard the queued playback tail
            silence = amplitude(recorder.record(numframes=24000))
        levels = dict(frequency_hz=997, baseline_amplitude=baseline, playing_amplitude=playing,
                      stopped_amplitude=silence, raw_audio_saved=False)
        assert playing > max(0.0001, baseline * 5), levels
        assert silence < playing * .2, levels
        return dict(native_cancel=True, vue_model_import=model, skeleton_nodes=skel['node_count'],
                    skeleton_bones=skel['bone_count'], ik_revision_conflict=True, ik_roundtrip=True,
                    vue_image_import=ui_image, vue_audio_import=audio, audio_play_receipt=played, audio_stop_receipt=stopped,
                    audio_output_loopback=levels)
    finally:
        for hwnd in dialog.windows():
            dialog.close(hwnd)
        web.socket.close()
