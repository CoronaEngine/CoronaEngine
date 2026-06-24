from __future__ import annotations

from typing import Any, Callable

from .generation_scheduler import GenerationJob


class SceneComposerJobRunner:
    """GenerationScheduler stage handler for SceneComposer.compose().

    This adapter is intentionally thin: the scheduler owns queueing/cancel/pause,
    SceneComposer owns progressive scene generation, and Coordinator context is
    passed as runtime-only kwargs so batch boundaries can flow back as events.
    """

    def __init__(self, composer_factory: Callable[[], Any]) -> None:
        self._composer_factory = composer_factory

    def compose(self, job: GenerationJob) -> dict[str, Any]:
        composer = self._composer_factory()
        seed_plan = job.payload.get("seed_plan") if isinstance(job.payload.get("seed_plan"), dict) else {}
        prompt = self._compose_prompt_from_payload(job.payload, seed_plan)
        if not prompt:
            raise ValueError("generation job has no prompt or SeedPlan intent_summary")
        if self._is_append_job(job.payload):
            self._limit_append_batch_size(composer, job.payload)
            prompt = self._append_prompt(prompt, job.payload)
        actor_id = self._target_actor_id(job.payload)
        result = composer.compose(
            prompt,
            do_import=bool(job.payload.get("do_import", True)),
            do_review=bool(job.payload.get("do_review", False)),
            progress_sink=job.runtime_context.get("progress_sink"),
            actor_id=actor_id,
            **job.compose_kwargs(),
        )
        if not isinstance(result, dict):
            result = {"compose_result": result}
        if result.get("paused"):
            mode = str(result.get("paused_mode") or "PAUSED")
            phase = str(result.get("paused_before_phase") or "")
            return {
                "compose_result": result,
                "paused": True,
                "paused_mode": mode,
                "paused_before_phase": phase,
            }
        return {"compose_result": result}

    @staticmethod
    def _compose_prompt_from_payload(payload: dict[str, Any], seed_plan: dict[str, Any]) -> str:
        if not isinstance(payload, dict):
            payload = {}
        if not isinstance(seed_plan, dict):
            seed_plan = {}
        if SceneComposerJobRunner._is_append_job(payload):
            return str(
                payload.get("prompt")
                or payload.get("intent_text")
                or seed_plan.get("design_brief")
                or seed_plan.get("intent_summary")
                or ""
            )
        brief = str(seed_plan.get("design_brief") or "").strip()
        if brief:
            items = [
                str(item or "").strip()
                for item in (seed_plan.get("design_items") or [])
                if str(item or "").strip()
            ]
            if items:
                return f"{brief}\n建议物体清单：{'、'.join(items[:12])}"
            return brief
        return str(
            payload.get("prompt")
            or payload.get("intent_text")
            or seed_plan.get("intent_summary")
            or ""
        )

    def stage_handlers(self) -> dict[str, Callable[[GenerationJob], Any]]:
        return {"compose": self.compose}

    @staticmethod
    def _is_append_job(payload: dict[str, Any]) -> bool:
        return bool(payload.get("append_mode") or str(payload.get("action_type") or "") == "post_generation_add")

    @staticmethod
    def _limit_append_batch_size(composer: Any, payload: dict[str, Any]) -> None:
        if not hasattr(composer, "max_items"):
            return
        try:
            requested = max(1, int(payload.get("max_items") or 2))
            current = int(getattr(composer, "max_items") or requested)
        except Exception:  # noqa: BLE001
            return
        try:
            setattr(composer, "max_items", max(1, min(current, requested)))
        except Exception:  # noqa: BLE001
            return

    @staticmethod
    def _append_prompt(prompt: str, payload: dict[str, Any]) -> str:
        contract = payload.get("scene_design_contract")
        style_prompt = ""
        if isinstance(contract, dict):
            style_prompt = str(contract.get("asset_style_prompt") or "").strip()
        parts = [
            "只追加本次新增对象，不重建整个场景，不覆盖已有布局。",
            f"新增请求：{prompt}",
        ]
        if style_prompt:
            parts.append(f"保持既有场景风格：{style_prompt}")
        return "\n".join(parts)

    @staticmethod
    def _target_actor_id(payload: dict[str, Any]) -> str:
        for key in ("actor_id", "target_actor_id"):
            value = str(payload.get(key) or "").strip()
            if value:
                return value
        latest = payload.get("latest_intervention")
        if isinstance(latest, dict):
            for key in ("actor_id", "target_actor_id"):
                value = str(latest.get(key) or "").strip()
                if value:
                    return value
        interventions = payload.get("pending_interventions")
        if isinstance(interventions, list):
            for item in reversed(interventions):
                if not isinstance(item, dict):
                    continue
                for key in ("actor_id", "target_actor_id"):
                    value = str(item.get(key) or "").strip()
                    if value:
                        return value
        return ""


__all__ = ["SceneComposerJobRunner"]
