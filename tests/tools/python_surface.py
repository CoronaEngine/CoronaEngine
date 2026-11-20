"""The exact surface the embedded Python module ``CoronaEngine`` publishes.

Two groups share the module, and the split is deliberate:

- ``HOST_SUPPORT_FUNCTIONS`` — the named entries the Python host itself needs: logging, input
  draining, the runtime phase and the project session. ``embedded_python`` calls all of them.
- ``HOST_SUPPORT_TYPES`` — the payload types those entries hand out. A type lives with the
  function that produces it, which is why ``InputEvent`` is top level.

The engine object model that used to sit under ``CoronaEngine.script`` is **gone**: it was the
script API of the retired script runtime, nothing in this repository imported it, and a module
whose whole surface is two groups is easier to reason about than one with a third that only
existed to be exported. ``ABSENT_MODULE_NAMES`` is the list that must not come back, so the
deletion cannot be half-undone by accident.

Written out by hand on purpose, exactly like ``native_surface.py``: if both this table and the
guard were derived from the sources, a new export would redefine the contract instead of
failing. Adding a name here is a decision about what Python may see.
"""

# The named host-support entries `CoronaEngine` publishes at the top level. `embedded_python`
# calls all of them; a name here that nothing calls is a binding waiting to rot.
HOST_SUPPORT_FUNCTIONS = frozenset({
    "active_project_path",
    "active_project_session",
    "drain_input_events",
    "python_runtime_phase",
    "register_python_script_service_dispatcher",
    "send_log",
    "unregister_python_script_service_dispatcher",
})

# Top-level payload types, with the member count each one publishes.
HOST_SUPPORT_TYPES = {
    "InputEvent": 15,
}

# The binding groups the module installs, in order. A third call is a third group, and it
# needs a name and a caller before it belongs on `CoronaEngine`.
BINDING_GROUPS = ("BindHostSupport", "BindCef")

# Names the deletion removed. `BindEngineObjectModel` bound the script object model under
# `.script`; `camera_follow_set_input_enabled` was the dead input-gate binding removed before
# it. Neither has a caller in this repository, so neither belongs on the module.
ABSENT_MODULE_NAMES = frozenset({
    "BindEngineObjectModel",
    "camera_follow_set_input_enabled",
    "Acoustics",
    "Actor",
    "ActorProfile",
    "Camera",
    "Environment",
    "Geometry",
    "Mechanics",
    "Optics",
    "Scene",
})
