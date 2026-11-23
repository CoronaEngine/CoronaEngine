"""Reference lists for the Python-face rules.

- ``ABSENT_MODULE_NAMES`` - names the object-model deletion removed; binding one again is a
  mistake, not a decision.
- ``BINDING_GROUPS`` - the group installers the module must call, in this order.
"""

# Group installers, in the order the module calls them: host support first, CEF transport second.
BINDING_GROUPS = ("BindHostSupport", "BindCef")

# The object model deleted with the script runtime; binding any of these again needs a decision.
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
