"""Inspect the local engine's CEF test endpoint (enable it only for the test run).

This exercises the deployed frontend and native manifest transport. Start the
engine with the thermal watchdog, CORONA_CEF_REMOTE_DEBUG_PORT=9222, and a low
CORONA_OPTICS_MAX_FPS for visual checks. It never launches an unguarded engine.
"""
import argparse
import base64
import json
from pathlib import Path
import time
import sys
from urllib.request import urlopen
import websocket


class Frontend:
    def __init__(self, port=9222, host="127.0.0.1"):
        if host not in ("127.0.0.1", "::1"):
            raise ValueError("Only the local engine endpoint is allowed")
        address = "[::1]" if host == "::1" else host
        with urlopen(f"http://{address}:{port}/json/list", timeout=3) as response:
            targets = [item for item in json.load(response) if item["type"] == "page"]
        if len(targets) != 1:
            raise RuntimeError(f"Expected one engine frontend, found {len(targets)}")
        if "/CabbageEditor/Frontend/" not in targets[0]["url"]:
            raise RuntimeError("Endpoint does not belong to the engine frontend")
        self.socket = websocket.create_connection(targets[0]["webSocketDebuggerUrl"], timeout=20,
                                                  suppress_origin=True)
        self.sequence = 0
        self.events = []
        self.call("Page.enable")

    def call(self, method, params=None):
        self.sequence += 1
        self.socket.send(json.dumps(dict(id=self.sequence, method=method, params=params or {})))
        while True:
            message = json.loads(self.socket.recv())
            if message.get("id") == self.sequence:
                if "error" in message:
                    raise RuntimeError(message["error"])
                return message.get("result", {})
            self.events.append(message)
            if message.get("method") == "Page.javascriptDialogOpening":
                raise RuntimeError(f"Frontend dialog: {message['params']}")

    def evaluate(self, expression, wait=True):
        result = self.call("Runtime.evaluate", dict(expression=expression, returnByValue=True,
                                                    awaitPromise=wait, timeout=15000, userGesture=True))
        if "exceptionDetails" in result:
            raise RuntimeError(result["exceptionDetails"])
        return result.get("result", {}).get("value")

    def api(self, name, args):
        request = json.dumps(dict(api=name, args=args))
        return self.evaluate("new Promise((resolve,reject)=>window.cefQuery({request:" + json.dumps(request) +
                             ",persistent:false,onSuccess:r=>resolve(JSON.parse(r)),onFailure:(c,m)=>reject(new Error(c+':'+m))}))")

    def click_text(self, text):
        # Derive the click location from the deployed DOM, then send a real CEF
        # pointer event so normal application handlers perform navigation.
        rectangle = self.evaluate("(()=>{const b=[...document.querySelectorAll('button')].find(e=>e.innerText.trim()==="+
                                  json.dumps(text)+");if(!b||b.disabled)throw Error('Button unavailable');const r=b.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2};})()")
        for kind in ["mousePressed", "mouseReleased"]:
            self.call("Input.dispatchMouseEvent", dict(type=kind, button="left", clickCount=1, **rectangle))
        time.sleep(.8)
        return self.evaluate("document.body.innerText")


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=9222)
    parser.add_argument("--host", choices=["127.0.0.1", "::1"], default="127.0.0.1")
    operations = parser.add_mutually_exclusive_group(required=True)
    operations.add_argument("--text", action="store_true")
    operations.add_argument("--click")
    operations.add_argument("--expression")
    operations.add_argument("--api")
    operations.add_argument("--screenshot", type=Path)
    parser.add_argument("--args", default="[]")
    args = parser.parse_args()
    frontend = Frontend(args.port, args.host)
    if args.text:
        result = frontend.evaluate("document.body.innerText")
    elif args.click:
        result = frontend.click_text(args.click)
    elif args.expression:
        result = frontend.evaluate(args.expression)
    elif args.api:
        result = frontend.api(args.api, json.loads(args.args))
    else:
        result = frontend.call("Page.captureScreenshot", dict(format="png"))
        args.screenshot.parent.mkdir(parents=True, exist_ok=True)
        args.screenshot.write_bytes(base64.b64decode(result["data"]))
        result = str(args.screenshot)
    print(json.dumps(result, ensure_ascii=False, indent=2))
