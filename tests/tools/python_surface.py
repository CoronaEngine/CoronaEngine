"""Reference lists for the Python-face rules.

These used to be the exact surface, compared name-for-name with the bindings. That comparison
is gone - it turned every binding addition into a two-file edit and its failure never said
which name was wrong. What is left here is data the rules need:

- ``ABSENT_MODULE_NAMES`` - the names the object-model deletion removed. The rules still refuse
  to let them come back, because a half-undone deletion is a mistake, not a decision.
- ``BINDING_GROUPS`` - the two group installers the module must call, in this order.

Nothing here is compared against the source as a *set*; the rules read the bindings directly.
``HOST_SUPPORT_FUNCTIONS`` is kept only as documentation of what that group is for.
"""

# The named host-support entries `CoronaEngine` publishes at the top level. `embedded_python`
# calls all of them; this list documents the group, it is not compared with the bindings.
# ``poll_service_requests`` and ``complete_service_request`` belong to the ``BindCef`` group,
# not here: the CEF transport owns the service queue the host loop drains.
HOST_SUPPORT_FUNCTIONS = frozenset({
    "active_project_path",
    "active_project_session",
    "drain_input_events",
    "python_runtime_phase",
    "register_python_script_service_dispatcher",
    "send_log",
    "should_stop",
    "unregister_python_script_service_dispatcher",
})

# The payload types those entries hand out. A type lives with the function that produces it,
# which is why ``InputEvent`` is top level.
HOST_SUPPORT_TYPES = {
    "InputEvent": 15,
}

# Group installers, in the order the module calls them: host support first, CEF transport second.
BINDING_GROUPS = ("BindHostSupport", "BindCef")

# The object model that used to sit under ``CoronaEngine.script`` is gone: it was the script API
# of the retired script runtime, nothing in this repository imported it, and its classes were the
# whole third group. `BindEngineObjectModel` bound it; `camera_follow_set_input_enabled` was the
# dead input-gate binding removed before it. None of these may be bound again without a decision.
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
