"""Feature flags for the Agent-native Runtime migration.

The flags are read-only process controls.  They do not switch LANChat traffic by
themselves; callers must explicitly consult them before routing user entrypoints
or allowing legacy workflow access.
"""

from __future__ import annotations

from dataclasses import dataclass
import os
from typing import MutableMapping, Mapping


TRUE_VALUES = {"1", "true", "yes", "on", "enabled"}
FALSE_VALUES = {"0", "false", "no", "off", "disabled"}


@dataclass(frozen=True)
class AgentRuntimeFlags:
    agent_runtime_enabled: bool = True
    old_workflow_direct_entry_disabled: bool = True
    allow_legacy_function_adapter: bool = True
    allow_legacy_main_workflow: bool = False
    use_scene_snapshot_provider: bool = False
    use_scene_review_provider: bool = False
    use_image_resource_provider: bool = False
    use_model_resource_provider: bool = False
    use_legacy_model_resource_provider: bool = False
    use_environment_component_provider: bool = False
    use_engine_environment_import_provider: bool = False
    use_engine_actor_import_provider: bool = False
    use_engine_actor_delete_provider: bool = False
    use_engine_layout_transform_provider: bool = False
    strict_image_to_model_pipeline: bool = False
    collaboration_runtime_write_enabled: bool = False

    @classmethod
    def from_env(cls, env: Mapping[str, str] | None = None) -> "AgentRuntimeFlags":
        values = env if env is not None else os.environ
        return cls(
            agent_runtime_enabled=_env_bool(values, "AGENT_RUNTIME_ENABLED", True),
            old_workflow_direct_entry_disabled=_env_bool(values, "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED", True),
            allow_legacy_function_adapter=_env_bool(values, "ALLOW_LEGACY_FUNCTION_ADAPTER", True),
            allow_legacy_main_workflow=_env_bool(values, "ALLOW_LEGACY_MAIN_WORKFLOW", False),
            use_scene_snapshot_provider=_env_bool(values, "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER", False),
            use_scene_review_provider=_env_bool(values, "AGENT_RUNTIME_USE_SCENE_REVIEW_PROVIDER", False),
            use_image_resource_provider=_env_bool(values, "AGENT_RUNTIME_USE_IMAGE_PROVIDER", False),
            use_model_resource_provider=_env_bool(values, "AGENT_RUNTIME_USE_MODEL_PROVIDER", False),
            use_legacy_model_resource_provider=_env_bool(values, "AGENT_RUNTIME_USE_LEGACY_MODEL_PROVIDER", False),
            use_environment_component_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENVIRONMENT_PROVIDER", False),
            use_engine_environment_import_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_ENVIRONMENT_IMPORT_PROVIDER", False),
            use_engine_actor_import_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER", False),
            use_engine_actor_delete_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_DELETE_PROVIDER", False),
            use_engine_layout_transform_provider=_env_bool(values, "AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER", False),
            strict_image_to_model_pipeline=_env_bool(values, "AGENT_RUNTIME_STRICT_IMAGE_TO_MODEL", False),
            collaboration_runtime_write_enabled=_env_bool(
                values,
                "AGENT_RUNTIME_ENABLE_COLLABORATION_WRITE",
                False,
            ),
        )

    def can_route_user_entry_to_runtime(self) -> bool:
        return self.agent_runtime_enabled

    def can_call_legacy_function_adapter(self) -> bool:
        return self.agent_runtime_enabled and self.allow_legacy_function_adapter

    def can_call_legacy_main_workflow(self) -> bool:
        return (
            self.agent_runtime_enabled
            and self.allow_legacy_main_workflow
            and not self.old_workflow_direct_entry_disabled
        )

    def assert_legacy_function_adapter_allowed(self) -> None:
        if not self.can_call_legacy_function_adapter():
            raise RuntimeError("legacy function adapters are disabled by AgentRuntimeFlags")

    def assert_legacy_main_workflow_blocked(self) -> None:
        if self.can_call_legacy_main_workflow():
            raise RuntimeError("legacy main workflow is enabled; this violates Agent-native migration invariants")

    def can_use_model_resource_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_model_resource_provider

    def can_use_legacy_model_resource_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_legacy_model_resource_provider

    def can_use_image_resource_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_image_resource_provider

    def can_use_environment_component_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_environment_component_provider

    def can_use_engine_environment_import_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_engine_environment_import_provider

    def can_use_scene_snapshot_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_scene_snapshot_provider

    def can_use_scene_review_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_scene_review_provider

    def can_use_engine_actor_import_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_engine_actor_import_provider

    def can_use_engine_actor_delete_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_engine_actor_delete_provider

    def can_use_engine_layout_transform_provider(self) -> bool:
        return self.can_call_legacy_function_adapter() and self.use_engine_layout_transform_provider

    def can_execute_collaboration_runtime_write(self) -> bool:
        return self.agent_runtime_enabled and self.collaboration_runtime_write_enabled


def install_f5_runtime_provider_env_defaults(env: MutableMapping[str, str] | None = None) -> None:
    """Install narrow Runtime provider defaults for the editor/F5 entrypoint.

    Unit tests and CLI callers can still pass explicit environments to
    ``AgentRuntimeFlags.from_env``.  This helper is intentionally opt-out via
    existing environment variables: explicit ``0/off/false`` values are
    preserved, while the editor plugin gets the minimum providers needed for
    the F5 Runtime bridge to attempt real engine writes.
    """

    target = env if env is not None else os.environ
    defaults = {
        "AGENT_RUNTIME_ENABLED": "1",
        "OLD_WORKFLOW_DIRECT_ENTRY_DISABLED": "1",
        "ALLOW_LEGACY_FUNCTION_ADAPTER": "1",
        "ALLOW_LEGACY_MAIN_WORKFLOW": "0",
        "AGENT_RUNTIME_USE_MODEL_PROVIDER": "1",
        "AGENT_RUNTIME_USE_IMAGE_PROVIDER": "1",
        "AGENT_RUNTIME_STRICT_IMAGE_TO_MODEL": "1",
        "AGENT_RUNTIME_USE_ENGINE_IMPORT_PROVIDER": "1",
        "AGENT_RUNTIME_USE_ENGINE_ENVIRONMENT_IMPORT_PROVIDER": "1",
        "AGENT_RUNTIME_USE_SCENE_SNAPSHOT_PROVIDER": "1",
        "AGENT_RUNTIME_USE_ENGINE_TRANSFORM_PROVIDER": "1",
    }
    for key, value in defaults.items():
        target.setdefault(key, value)


def _env_bool(env: Mapping[str, str], name: str, default: bool) -> bool:
    raw = env.get(name)
    if raw is None:
        return default
    normalized = str(raw).strip().lower()
    if normalized in TRUE_VALUES:
        return True
    if normalized in FALSE_VALUES:
        return False
    return default
