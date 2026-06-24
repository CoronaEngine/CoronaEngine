import logging
from typing import Dict, Any, List



try:
    from Quasar.ai_service.entrance import ai_entrance
    from Quasar.ai_config.ai_config import reload_ai_config

    @ai_entrance.collector.register_setting("chat")
    def CHAT_SETTINGS() -> Dict[str, Any]:
        return {
            "provider": "dmxapi",
            "model": "gpt-5.5",
            "layout_model": "gpt-5.5",
            "system_prompt": """你是一个 AI 助手，可以帮助用户完成各种任务。""",
        }

    @ai_entrance.collector.register_setting("providers")
    def PROVIDERS() -> List[Dict[str, Any]]:
        return [
            {
                "name": "deepseek",
                "type": "openai-compatible",
                "base_url": "https://api.deepseek.com/v1",
                "api_key": "sk-94033d0c8aee425c802a07689cad69d6",
            },
            {
                "name": "dmx_image",
                "type": "dmx",
                "base_url": "https://www.dmxapi.cn/v1/images/generations",
                "api_key": "sk-qWQZAs7rbZewSJ78Ac50qjtPU4Dwln8XYMnmOCJzg0hGR4FA",
            },
            # {
            #     "name": "grsai_image",
            #     "type": "grsai",
            #     "base_url": "https://grsai.dakka.com.cn/v1/api/generate",
            #     "api_key": "sk-13e382852c4948f6b5c717535a5a1a0c",
            # },
            {
                "name": "dmxapi",
                "type": "openai-compatible",
                "base_url": "https://www.dmxapi.cn/v1",
                "api_key": "sk-qWQZAs7rbZewSJ78Ac50qjtPU4Dwln8XYMnmOCJzg0hGR4FA",
            },
        ]

    @ai_entrance.collector.register_setting("image")
    def IMAGE_SETTINGS() -> Dict[str, Any]:
        return {
            "enable": True,
            "provider": "dmx_image",
            "model": "gpt-image-2",
            "base_url": "https://www.dmxapi.cn/v1/images/generations",
        }

    @ai_entrance.collector.register_setting("omni")
    def OMNI_SETTINGS() -> Dict[str, Any]:
        return {
            "enable": True,
            "provider": "dmxapi",
            "model": "gpt-5.5",
            "request_timeout": 180.0,
            "image_detail": "auto",
        }

    @ai_entrance.collector.register_setting("hunyuan3d")
    def HUNYUAN_3D_SETTINGS() -> Dict[str, Any]:
        return {
            "enable": True,
            "api_key": "sk-y41uwqZkTgipsViywZNh3NtjOBQHWizKivmNJI72R8qGj8MJ",
            "api_keys": [
                "sk-JW5sgRCe3XSlgTXh5Xt25WVOsDG3os6eg19jutpvjYG0kBrM",
                "sk-KfygRCIuidGhv1aMD9txpzbukwUIn2fGCw1cxQAp7yv3OTcD"
            ],
            "region": "ap-guangzhou",
            "endpoint": "api.ai3d.cloud.tencent.com",
            "version": "pro",
            "result_format": "GLB",
            "enable_pbr": True,
            "model": "3.0",
            "generate_type": "Normal",
            "face_count": 500000,
            "request_timeout": 300.0,
            "poll_interval": 3.0,
            "poll_timeout": 600.0,
            "max_concurrent_generations": 2,
        }

    reload_ai_config()

    # 强制工具注册表重新发现（用户配置可能在 warmup 之后加载）
    from Quasar.ai_tools.registry import get_tool_registry
    get_tool_registry().reset_discovery()
    from Quasar.ai_agent.executor import reset_cached_agent
    reset_cached_agent()

except ImportError as e:
    logging.error(e)
