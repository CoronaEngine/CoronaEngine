"""CPU checks for native viewport recovery, without a running engine."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "restore_native_viewport.py"
sys.path.insert(0, str(SCRIPT.parent))


class RestoreNativeViewportTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "Native viewport recovery tool is missing")
        spec = importlib.util.spec_from_file_location("restore_viewport", SCRIPT)
        self.tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.tool)

    def observe(self, rectangle, scale=1, surface=True, bridge=True, pending=False):
        config = json.dumps(dict(rectangle=rectangle, scale=scale, surface=surface,
                                 bridge=bridge, pending=pending))
        javascript = "const c=" + config + r""";
const events=[];
globalThis.window={devicePixelRatio:c.scale,
  __coronaNativeViewportReloadPending:c.pending,
  dispatchEvent:event=>events.push(event.type),
  coronaBridge:c.bridge?{setCameraViewport:()=>{throw Error('Direct viewport override')}}:null};
globalThis.document={querySelector:selector=>{
  if(selector!=='[data-viewport-pick-surface]')throw Error('Wrong viewport surface');
  return c.surface?{getBoundingClientRect:()=>c.rectangle}:null;
}};
globalThis.requestAnimationFrame=callback=>callback();
const result=await eval(""" + json.dumps(self.tool.VIEWPORT_SCRIPT) + """ );
console.log(JSON.stringify({result,events}));"""
        result = subprocess.run(["node", "--input-type=module", "-e", javascript],
                                check=True, capture_output=True, text=True)
        return json.loads(result.stdout)

    def test_full_surface_and_resize_preserve_native_aspect_at_each_dpi(self):
        cases = [({"left": 0, "top": 0, "width": 1920, "height": 1009}, 1,
                  {"x": 0, "y": 0, "width": 1920, "height": 1009}),
                 ({"left": 13, "top": 7, "width": 1920, "height": 1080}, 1,
                  {"x": 13, "y": 7, "width": 1920, "height": 1080}),
                 ({"left": 5, "top": 3, "width": 1920, "height": 1009}, 2,
                  {"x": 10, "y": 6, "width": 3840, "height": 2018})]
        for rectangle, scale, expected in cases:
            with self.subTest(scale=scale, rectangle=rectangle):
                observed = self.observe(rectangle, scale)
                self.assertEqual(observed, {"result": {"ready": True,
                    "device_pixel_ratio": scale, "physical_rect": expected}, "events": ["resize"]})

    def test_missing_surface_bridge_or_new_reload_context_stays_unready(self):
        rectangle = {"left": 0, "top": 0, "width": 1920, "height": 1009}
        for options in ({"surface": False}, {"bridge": False}, {"pending": True}):
            with self.subTest(options=options):
                self.assertEqual(self.observe(rectangle, **options),
                                 {"result": {"ready": False}, "events": []})


if __name__ == "__main__":
    unittest.main()
