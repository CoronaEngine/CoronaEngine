import json
import logging
import sys
import threading
import time
from pathlib import Path

from runtime.plugin_base import PluginBase


logger = logging.getLogger(__name__)
_AITOOL_DIR = Path(__file__).resolve().parent
if str(_AITOOL_DIR) not in sys.path:
    sys.path.insert(0, str(_AITOOL_DIR))

# The registry publishes the lightweight AITool facade before this service is ready.
INITIALIZE_AFTER_PUBLISH = True


def _create_lanchat_scene_composer():
    from .cai_extensions.agent.scene_composer import SceneComposer

    return SceneComposer(scene_name="Scene/default.scene")


def initialize_script_service(stop_token=None):
    """Initialize the runtime after the lightweight AITool facade is published."""
    from .configuration.local_secrets import load_ai_setting

    load_ai_setting()
    if stop_token is not None and stop_token.is_set():
        return False
    return AITool.initialize_runtime(stop_token=stop_token)


@PluginBase.register_web("AITool")
class AITool(PluginBase):
    _STREAM_QUEUE_MAXSIZE = 128
    _runtime_lock = threading.RLock()
    _runtime_state = "cold"
    _runtime_error = ""
    _runtime_done = threading.Event()
    _runtime_stop = threading.Event()

    _executor = None
    _request_service = None
    _media_ingress = None
    _stream_dispatcher = None
    _cai_app = None
    _cai_client = None
    _event_loop_runner = None
    _controller = None
    _lanchat_agent_worker = None
    _request_states = None
    _hint_service = None
    _node_graph_review_service = None
    _node_graph_review_chat_service = None
    _node_graph_generation_service = None
    _cabbage_context_service = None
    _build_error_response = None

    @classmethod
    def runtime_state(cls):
        with cls._runtime_lock:
            return cls._runtime_state, cls._runtime_error

    @classmethod
    def _initializing_response(cls):
        state, error = cls.runtime_state()
        if state == "failed":
            return {
                "success": False,
                "status": "degraded",
                "message": f"AITool initialization failed: {error}",
            }
        return {
            "success": False,
            "status": "initializing",
            "message": "AITool is initializing",
        }

    @classmethod
    def _build_runtime(cls):
        from concurrent.futures import ThreadPoolExecutor

        from api.editor_api import emit_compat_editor_event
        from Quasar.ai_service.entrance import get_ai_entrance
        from Quasar.ai_tools.common import build_error_response
        from Quasar.cai import CAIApp

        from .cai_extensions.register import install as install_cai_extensions
        from .services.ai_hint_service import get_hint_service
        from .services.ai_plugin_controller import AIPluginController
        from .services.cabbage_context_service import get_cabbage_context_service
        from .services.cai_client import CAIClient
        from .services.event_loop_runner import EventLoopRunner
        from .services.lanchat_agent_worker import LANChatAgentWorker
        from .services.media_ingress import MediaIngress
        from .services.node_graph_generation_service import get_node_graph_generation_service
        from .services.node_graph_review_chat_service import get_node_graph_review_chat_service
        from .services.node_graph_review_service import get_node_graph_review_service
        from .services.request_service import AIRequestService
        from .services.stream_dispatcher import StreamDispatcher
        from .services.media_storage import base64_to_image_file, upload_file_to_server
        from .services.local_file_service import LocalFileService

        from .services.agent_runtime.flags import install_f5_runtime_provider_env_defaults

        install_f5_runtime_provider_env_defaults()
        cls._executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="AI_")
        cls._request_service = AIRequestService()
        cls._media_ingress = MediaIngress(base64_to_image_file, upload_file_to_server)
        cls._stream_dispatcher = StreamDispatcher(emit_compat_editor_event)
        cai_started = time.perf_counter()
        cls._cai_app = CAIApp.from_legacy_entrance(lambda: get_ai_entrance())
        logger.info(
            "AITool CAI runtime constructed in %.1fms thread=%s",
            (time.perf_counter() - cai_started) * 1000.0,
            threading.get_ident(),
        )
        install_cai_extensions(cls._cai_app)
        cls._cai_client = CAIClient(cls._cai_app, cls._executor, cls._STREAM_QUEUE_MAXSIZE)
        cls._event_loop_runner = EventLoopRunner()
        cls._build_error_response = build_error_response
        cls._controller = AIPluginController(
            cls._request_service,
            cls._media_ingress,
            cls._stream_dispatcher,
            cls._cai_client,
            cls._event_loop_runner,
            build_error_response,
        )
        cls._lanchat_agent_worker = LANChatAgentWorker(
            composer_factory=_create_lanchat_scene_composer,
            async_agent_execution=True,
        )
        cls._request_states = cls._request_service.states
        cls._hint_service = get_hint_service()
        cls._node_graph_review_service = get_node_graph_review_service()
        cls._node_graph_review_chat_service = get_node_graph_review_chat_service()
        cls._node_graph_generation_service = get_node_graph_generation_service()
        cls._cabbage_context_service = get_cabbage_context_service()

    @classmethod
    def initialize_runtime(cls, stop_token=None):
        with cls._runtime_lock:
            if cls._runtime_state == "ready":
                return True
            if cls._runtime_state == "initializing":
                return cls._runtime_done.wait(0.0)
            if cls._runtime_state in {"failed", "stopped"}:
                return False
            cls._runtime_state = "initializing"
            cls._runtime_error = ""
            cls._runtime_stop.clear()
            cls._runtime_done.clear()

        started = time.perf_counter()
        worker_started = False
        try:
            if stop_token is not None and stop_token.is_set():
                return False
            cls._build_runtime()
            runtime_ms = (time.perf_counter() - started) * 1000.0
            logger.info("AITool runtime constructed in %.1fms thread=%s", runtime_ms, threading.get_ident())
            if stop_token is not None and stop_token.is_set():
                return False

            worker_started_at = time.perf_counter()
            cls._lanchat_agent_worker.start()
            worker_started = True
            logger.info(
                "AITool LANChat worker started in %.1fms thread=%s",
                (time.perf_counter() - worker_started_at) * 1000.0,
                threading.get_ident(),
            )
            cls._init_hint_service()

            warmup_started = time.perf_counter()
            result = cls._run_warmup(stop_token or cls._runtime_stop)
            logger.info(
                "AITool warmup finished in %.1fms thread=%s result=%s",
                (time.perf_counter() - warmup_started) * 1000.0,
                threading.get_ident(),
                bool(result),
            )
            if result is False or (stop_token is not None and stop_token.is_set()):
                return False
            cls._start_optional_p0_test()
            with cls._runtime_lock:
                cls._runtime_state = "ready"
            logger.info(
                "AITool runtime ready in %.1fms thread=%s",
                (time.perf_counter() - started) * 1000.0,
                threading.get_ident(),
            )
            return True
        except Exception as error:
            with cls._runtime_lock:
                cls._runtime_error = str(error)
                cls._runtime_state = "failed"
            logger.exception("AITool runtime initialization failed")
            return False
        finally:
            if not worker_started and cls._lanchat_agent_worker is not None:
                cls._lanchat_agent_worker = None
            cls._runtime_done.set()

    @classmethod
    def _run_warmup(cls, stop_token):
        from Quasar.ai_tools import warmup as warmup_module

        phase_names = (
            "warmup_configs",
            "warmup_storage",
            "warmup_http_clients",
            "warmup_account_pools",
            "warmup_tools",
            "warmup_workflows",
            "warmup_agent",
        )
        originals = {name: getattr(warmup_module, name) for name in phase_names}

        def timed_phase(name, phase):
            def run_phase(*args, **kwargs):
                started = time.perf_counter()
                logger.info(
                    "AITool warmup phase started phase=%s thread=%s",
                    name,
                    threading.get_ident(),
                )
                try:
                    result = phase(*args, **kwargs)
                    logger.info(
                        "AITool warmup phase finished phase=%s duration_ms=%.1f success=true thread=%s",
                        name,
                        (time.perf_counter() - started) * 1000.0,
                        threading.get_ident(),
                    )
                    return result
                except Exception:
                    logger.exception(
                        "AITool warmup phase failed phase=%s duration_ms=%.1f thread=%s",
                        name,
                        (time.perf_counter() - started) * 1000.0,
                        threading.get_ident(),
                    )
                    raise

            return run_phase

        for name, phase in originals.items():
            setattr(warmup_module, name, timed_phase(name, phase))
        try:
            return warmup_module.warmup_all(stop_token=stop_token)
        finally:
            for name, phase in originals.items():
                setattr(warmup_module, name, phase)

    @classmethod
    def _start_optional_p0_test(cls):
        import os

        if os.environ.get("CORONA_P0_CONCURRENCY_TEST") != "1":
            return
        import threading as threading_module

        cabbage_root = _AITOOL_DIR.parent
        if str(cabbage_root) not in sys.path:
            sys.path.insert(0, str(cabbage_root))
        from test_engine_concurrency import main as p0_main

        def run_p0_test():
            import time as time_module

            time_module.sleep(3)
            p0_main()

        threading_module.Thread(target=run_p0_test, name="P0-ConcurrencyTest", daemon=True).start()

    @classmethod
    def _init_hint_service(cls) -> None:
        def ai_caller(prompt: str) -> str | None:
            try:
                from Quasar.cai.protocol.request import ChatRequest

                req = ChatRequest.from_text(
                    text=prompt,
                    metadata={"hint_generation": True, "skip_conversation_store": True},
                )
                chunks = cls._cai_app.chat(req)
                text = "".join(chunks).strip().strip('"\'').strip()
                return text if text else None
            except Exception as exc:
                logger.debug("AI hint generation failed: %s", exc)
                return None

        cls._hint_service.set_ai_caller(ai_caller)

    @classmethod
    def send_message_to_ai_stream(cls, ai_message: str) -> None:
        if cls.runtime_state()[0] != "ready":
            return None
        cls._controller.send_message_to_ai_stream(ai_message)

    @classmethod
    def generate_hint(cls, element_type: str, context: dict = None) -> str:
        if cls.runtime_state()[0] != "ready":
            return "继续探索编辑器吧！"
        try:
            return cls._hint_service.generate_hint(element_type, context or {})
        except Exception as error:
            logger.error("Error generating hint: %s", error)
            return "继续探索编辑器吧！"

    @classmethod
    def submit_request(cls, request) -> dict:
        if cls.runtime_state()[0] != "ready":
            return cls._initializing_response()
        try:
            parsed = json.loads(request) if isinstance(request, str) else request
        except Exception:
            parsed = request
        if isinstance(parsed, dict):
            operation = parsed.get("operation")
            service = {
                "node_graph.review.start": cls._node_graph_review_service.start,
                "node_graph.review.status": cls._node_graph_review_service.status,
                "node_graph.review.chat": cls._node_graph_review_chat_service.chat,
                "node_graph.review.chat.start": cls._node_graph_review_chat_service.start,
                "node_graph.review.chat.status": cls._node_graph_review_chat_service.status,
                "node_graph.review.chat.cancel": cls._node_graph_review_chat_service.cancel,
                "node_graph.generate.start": cls._node_graph_generation_service.start,
                "node_graph.generate.status": cls._node_graph_generation_service.status,
                "node_graph.generate.cancel": cls._node_graph_generation_service.cancel,
                "cabbage.context.load": cls._cabbage_context_service.load,
                "cabbage.context.record_event": cls._cabbage_context_service.record_event,
                "cabbage.context.update_task": cls._cabbage_context_service.update_task,
                "cabbage.context.append_message": cls._cabbage_context_service.append_message,
                "cabbage.goal_plan.start": cls._cabbage_context_service.start_goal_plan,
                "cabbage.goal_plan.status": cls._cabbage_context_service.goal_plan_status,
                "cabbage.profile.score.start": cls._cabbage_context_service.start_score_update,
                "cabbage.profile.score.status": cls._cabbage_context_service.score_update_status,
            }.get(operation)
            if service is not None:
                if operation.endswith("status") or operation.endswith("cancel"):
                    return service(parsed.get("taskId") or parsed.get("task_id") or "")
                return service(parsed.get("payload") or {})
        return cls._controller.submit_request(request)

    @classmethod
    def request_shutdown(cls):
        cls._runtime_stop.set()
        worker = cls._lanchat_agent_worker
        if worker is not None:
            worker.stop()

    @classmethod
    def cleanup(cls):
        with cls._runtime_lock:
            if cls._runtime_state == "stopped":
                return
            cls._runtime_state = "stopped"
        cls.request_shutdown()
        for service in (
            cls._node_graph_review_service,
            cls._node_graph_review_chat_service,
            cls._node_graph_generation_service,
            cls._cabbage_context_service,
        ):
            if service is not None:
                service.shutdown()
        if cls._controller is not None and cls._executor is not None:
            cls._controller.cleanup(cls._executor)
        cls._runtime_done.set()

    @staticmethod
    def read_local_file_as_base64(file_url: str) -> str:
        from .services.local_file_service import LocalFileService

        return LocalFileService.read_as_base64(file_url)
