import os
import logging
import time
from typing import Any, Dict, List

from Quasar.ai_models.base_pool import MediaCategory, OmniRequest, get_pool_registry

from Quasar.ai_workflow.streaming import stream_output_node
from .formatters import NO_OUTPUT
from ..workflow_test_support import get_test_case

logger = logging.getLogger(__name__)

_MAX_REVIEW_RETRIES = 2


def _build_review_prompt(prompt_text: str) -> str:
    """构建六视图视觉质检提示词。"""
    return (
        f"你是一位极其严苛的 3D 模型视觉质检专家。你的任务是通过检查同一 3D 模型的【六个正交视角图】（前、后、左、右、顶、底），"
        f"来判定该模型是否合格。\n\n"
        f"【目标提示词】\n{prompt_text}\n\n"
        f"【视觉特征检查清单】（请严格按照以下具体的视觉特征进行排查）：\n\n"
        f"1. 多视角逻辑冲突（致命错误 - 重点检查！）：\n"
        f"   - 两面神现象（Janus Problem）：仔细对比“前视图”和“后视图”。后脑勺绝对不能长出第二张脸、五官或原本只该在正面的配饰！\n"
        f"   - 肢体增生/缺失：对比左右视图，人类/动物的四肢数量必须正常。不能出现三只手、多余的腿，或者某条腿在侧视图中凭空消失。\n"
        f"   - 物理位置错乱：如果前视图中角色右手拿着武器，后视图对应位置也必须有武器，且不能像纸片一样没有厚度。\n\n"
        f"2. 穿模与黏连（交接处检查）：\n"
        f"   - 融化感：观察肢体与躯干、衣服与皮肤、手与物品的交界处。如果它们像橡皮泥一样互相“融化”在一起，没有清晰的物理边界，视为严重穿模。\n"
        f"   - 贯穿：如果武器直接刺穿了身体，或者头发直直插进肩膀里，视为不合格。\n\n"
        f"3. 悬空与表面碎片（背景与边缘检查）：\n"
        f"   - 漂浮物：模型周围、半空中绝对不能有孤立的色块、无意义的几何碎片或不相干的漂浮物体。\n"
        f"   - 表面破损：模型表面应该平滑或符合材质纹理。如果出现密集的坑洞、撕裂状的锯齿边缘、或者像刺猬一样的不规则尖刺，视为网格崩坏。\n\n"
        f"4. 语义与结构还原度：\n"
        f"   - 模型的主体身份、核心特征必须与【目标提示词】高度一致。如果提示词是汽车，不能生成带轮子的沙发。\n"
        f"   - 比例不能严重失调（例如头比身体大3倍，除非提示词明确要求Q版）。\n\n"
        f"【输出规范】（极其重要，决定系统是否会崩溃）\n"
        f"不要包含任何寒暄、思考过程或多余的标点符号。请严格只输出两行文本：\n"
        f"- 如果没有发现上述任何问题（完全合格）：\n"
        f"  第1行严格输出：PASS\n"
        f"  第2行严格输出：符合预期\n"
        f"- 如果发现上述任意一种缺陷（不合格）：\n"
        f"  第1行严格输出：FAIL\n"
        f"  第2行请用一句话指出具体视觉缺陷（例如：FAIL\\n后视图出现了第二张脸，且左侧有悬空碎片）\n\n"
        f"最后警告：在判定为合格的评价中，绝对、绝对不要出现 'FAIL' 这个单词！"
    )


def _collect_view_image_urls(views_dict: Dict[str, str]) -> List[str]:
    """提取可用六视图路径，按固定视角顺序优先。"""
    ordered_keys = ("front", "back", "left", "right", "top", "bottom")
    image_urls: List[str] = []

    for key in ordered_keys:
        img_path = views_dict.get(key)
        if isinstance(img_path, str) and img_path.strip() and os.path.exists(img_path):
            image_urls.append(img_path)

    for key, img_path in views_dict.items():
        if key in ordered_keys:
            continue
        if isinstance(img_path, str) and img_path.strip() and os.path.exists(img_path):
            image_urls.append(img_path)

    return image_urls


def _call_omni_visual_review(
    *,
    session_id: str,
    prompt_text: str,
    views_dict: Dict[str, str],
) -> str:
    """通过 Omni 池调用多模态模型执行六视图视觉质检。"""
    image_urls = _collect_view_image_urls(views_dict)
    if not image_urls:
        raise ValueError("无可用六视图图片")

    pool_registry = get_pool_registry()
    request = OmniRequest(
        session_id=session_id or f"model-retrieval-review-{int(time.time())}",
        prompt=_build_review_prompt(prompt_text),
        image_urls=image_urls,
    )
    task = pool_registry.create_task(MediaCategory.OMNI, request)
    if task is None:
        raise RuntimeError("Omni 池无可用账号，无法执行视觉审查")

    result = task()
    return str(result.metadata.get("analysis_result", "") or "").strip()


def _get_mock_visual_review_reply(
    state: Dict[str, Any],
    result: Dict[str, Any],
) -> str | None:
    """在 workflow_test 模式下返回测试样例指定的视觉审查结果。"""
    metadata = state.get("metadata", {}) or {}
    if not metadata.get("workflow_test"):
        return None

    explicit_reply = str(result.get("mock_visual_review_reply", "") or "").strip()
    if explicit_reply:
        return explicit_reply

    decision = str(result.get("mock_visual_review", "") or "").strip().upper()
    if decision == "PASS":
        return "PASS\n符合预期"
    if decision == "FAIL":
        reason = str(result.get("mock_visual_review_reason", "") or "未通过测试态视觉审查").strip()
        return f"FAIL\n{reason}"

    test_case = get_test_case(metadata.get("workflow_test_case", "default"))
    expected_results = test_case.get("expected_model_results", [])
    if not isinstance(expected_results, list):
        return None

    actor_name = str(result.get("item_name", "") or "").strip()
    object_id = str(result.get("object_id", "") or "").strip()
    for expected in expected_results:
        if not isinstance(expected, dict):
            continue
        if (
            str(expected.get("item_name", "") or "").strip() == actor_name
            and str(expected.get("object_id", "") or "").strip() == object_id
        ):
            explicit_reply = str(expected.get("mock_visual_review_reply", "") or "").strip()
            if explicit_reply:
                return explicit_reply

            decision = str(expected.get("mock_visual_review", "") or "").strip().upper()
            if decision == "PASS":
                return "PASS\n符合预期"
            if decision == "FAIL":
                reason = str(expected.get("mock_visual_review_reason", "") or "未通过测试态视觉审查").strip()
                return f"FAIL\n{reason}"

    return None


# 视觉审查开关：设为 True 时跳过 AI 模型调用，直接放行所有结果
_VISUAL_REVIEW_DISABLED = True


@stream_output_node("integrated", NO_OUTPUT)
def visual_review_node(state: Dict[str, Any]) -> Dict[str, Any]:
    model_results = state.get("model_results", [])
    session_id = str(state.get("session_id", "") or "")

    if _VISUAL_REVIEW_DISABLED:
        logger.info("[Workflow][visual_review] 审查阶段已关闭，全部放行。")
        for result in model_results:
            if not result.get("review_passed") and not result.get("error"):
                result["review_passed"] = True
        return {"model_results": model_results, "needs_retry": False}

    needs_retry = False
    pending_generation = []

    for result in model_results:
        # 如果已经审查通过，或直接从库里检索到的，跳过
        if result.get("review_passed") or result.get("source") == "retrieval" or result.get("error"):
            continue

        actor_name = result.get("object_id") or result.get("item_name")
        prompt_text = result.get("image_prompt") or result.get("item_name")
        # 仅使用本轮捕获的六视图，不再回退到历史 state.six_view_images
        views_dict = result.get("six_views_dict", {})

        if not views_dict:
            logger.warning(f"[Workflow] 未找到 {actor_name} 的六视图数据，跳过校验。")
            result["review_passed"] = True
            continue

        try:
            logger.info(
                f"[Workflow] 正在通过 Omni 池将 {actor_name} 的六视图送入视觉大模型校验..."
            )

            raw_reply = _get_mock_visual_review_reply(state, result)
            if raw_reply is None:
                raw_reply = _call_omni_visual_review(
                    session_id=session_id,
                    prompt_text=prompt_text,
                    views_dict=views_dict,
                )
            reply_lines = [line.strip() for line in raw_reply.splitlines() if line.strip()]
            decision = reply_lines[0].upper() if reply_lines else "PASS"
            review_reason = reply_lines[1] if len(reply_lines) > 1 else raw_reply or "未提供原因"

            if decision.startswith("FAIL"):
                logger.warning(f"[Workflow] {actor_name} 视觉审查不通过: {raw_reply}")

                retry_count = result.get("retry_count", 0)
                if retry_count >= _MAX_REVIEW_RETRIES:
                    logger.error(
                        f"[Workflow] {actor_name} 达到最大重试次数，停止重试。"
                    )
                    result["review_passed"] = False
                    result["error"] = f"视觉审查未通过: {review_reason}"
                else:
                    result["review_passed"] = False
                    result["source"] = "pending_generation"
                    result["retry_count"] = retry_count + 1
                    result["review_reason"] = review_reason
                    pending_generation.append(dict(result))
                    needs_retry = True
            else:
                logger.info(f"[Workflow] {actor_name} 视觉审查完美通过！结果: {raw_reply}")
                result["review_passed"] = True
                result.pop("review_reason", None)
                result["source"] = "generation"

        except Exception as e:
            logger.error(
                f"[Workflow] Omni 视觉审查请求失败，跳过审查: {e}", exc_info=True
            )
            # 为了防止工作流卡死，请求失败时默认放行
            result["review_passed"] = True
            result["source"] = "generation"

    return {
        "model_results": model_results,
        "needs_retry": needs_retry,
        "intermediate": {
            **state.get("intermediate", {}),
            "pending_generation": pending_generation,
        },
    }
