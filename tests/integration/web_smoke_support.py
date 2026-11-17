"""Shared helpers for the deployed-editor smoke probes.

`open_web_project` opens a project through the production launcher object and
`wait_for` polls a read until it matches. The interpreter probe and its
`dispatch_script_request_from_cpp` hook that used to live here went away with the
script runtime: nothing called them, and the text dispatcher they patched no
longer exists.
"""
import json
import time


def open_web_project(web, path):
    """Use the production Web owner so old-window saves finish before replacement."""
    return web.evaluate("""(async()=>{
      const module=await import(document.querySelector('script[type=module]').src);
      const launcher=Object.values(module).find(value=>typeof value?.open_project==='function'
        &&!value.create_world_project);
      if(!launcher)throw Error('Production project launcher missing');
      return launcher.open_project(""" + json.dumps(str(path)) + ");})()")


def wait_for(read, condition, description):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        value = read()
        if condition(value):
            return value
        time.sleep(0.1)
    raise AssertionError(f"{description}: {value!r}")
