"""Restore an existing engine's native interactive viewport after fixed-size tests.

Usage: python tools/restore_native_viewport.py --port 9297 [--target-id ID]
Reloads the frontend to discard injected viewport wrappers, then reports its
full DOM rectangle in physical pixels. The renderer owns the render extent.
"""
import argparse
import json
import time

from vision_frontend_probe import Frontend


VIEWPORT_SCRIPT = """(()=>{
const surface=document.querySelector('[data-viewport-pick-surface]');
if(window.__coronaNativeViewportReloadPending || !surface ||
   typeof window.coronaBridge?.setCameraViewport!=='function')return {ready:false};
const r=surface.getBoundingClientRect(), s=Math.max(Number(window.devicePixelRatio||1),0.01);
if(r.width<=0 || r.height<=0)return {ready:false};
window.dispatchEvent(new Event('resize'));
return new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve({
  ready:true,device_pixel_ratio:s,physical_rect:{
    x:Math.max(0,Math.round(r.left*s)),y:Math.max(0,Math.round(r.top*s)),
    width:Math.max(1,Math.round(r.width*s)),height:Math.max(1,Math.round(r.height*s))
  }
}))));
})()"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=9297)
    parser.add_argument("--target-id")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    frontend = Frontend(port=args.port, target_id=args.target_id)
    try:
        # A reload response can precede navigation. Only the new V8 context
        # loses this marker, so the old wrapped page cannot report readiness.
        frontend.evaluate("window.__coronaNativeViewportReloadPending=true")
        frontend.call("Page.reload")
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                result = frontend.evaluate(VIEWPORT_SCRIPT)
            except RuntimeError as error:
                if "context" not in str(error).lower():
                    raise
                result = None
            if isinstance(result, dict) and result.get("ready"):
                print(json.dumps(result, ensure_ascii=False))
                return 0
            time.sleep(.1)
        raise TimeoutError("Native MainPage viewport was not ready after reload")
    finally:
        frontend.socket.close()


if __name__ == "__main__":
    raise SystemExit(main())
