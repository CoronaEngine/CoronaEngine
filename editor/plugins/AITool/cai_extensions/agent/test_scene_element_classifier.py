from __future__ import annotations

from scene_element_classifier import (
    LAYOUT,
    MODEL,
    SUBSTRATE,
    TEXT_TO_3D_PREFERRED,
    route_model_items,
    summarize_classification,
)


def test_substrate_terms_route_away_from_model_generation() -> None:
    model_items, routed = route_model_items(
        "做一个草原夜晚幻想集市，有天空、石板地面、摊位和灯笼",
        [
            {"name": "草原"},
            {"name": "天空"},
            {"name": "石板地面"},
            {"name": "摊位"},
            {"name": "灯笼"},
        ],
    )
    assert [item["name"] for item in model_items] == ["摊位", "灯笼"]
    by_name = {item.name: item for item in routed}
    assert by_name["草原"].target_pipeline == SUBSTRATE
    assert by_name["天空"].target_pipeline == SUBSTRATE
    assert by_name["石板地面"].target_pipeline == SUBSTRATE
    assert by_name["摊位"].target_pipeline == MODEL


def test_layout_terms_are_disclosed_but_not_model_generated() -> None:
    model_items, routed = route_model_items(
        "室内展示区连接户外庭院，要有入口过渡、连接动线、展示桌",
        [
            {"name": "室内展示区"},
            {"name": "户外庭院"},
            {"name": "入口过渡"},
            {"name": "连接动线"},
            {"name": "展示桌"},
        ],
    )
    assert [item["name"] for item in model_items] == ["展示桌"]
    by_name = {item.name: item for item in routed}
    assert by_name["入口过渡"].target_pipeline == LAYOUT
    assert by_name["连接动线"].target_pipeline == LAYOUT
    summary = summarize_classification(routed)
    assert "准备生成模型：展示桌" in summary
    assert "环境/地形：户外庭院" in summary
    assert "布局结构：室内展示区、入口过渡、连接动线" in summary


def test_thin_net_assets_avoid_image_to_3d_and_decals_route_to_substrate() -> None:
    model_items, routed = route_model_items(
        "做一个二战前线，有沙袋、铁丝网障碍、栅栏、海报、森林和天空",
        [
            {"name": "沙袋"},
            {"name": "铁丝网障碍"},
            {"name": "栅栏"},
            {"name": "海报"},
            {"name": "森林"},
            {"name": "天空"},
        ],
    )
    by_name = {item.name: item for item in routed}
    model_by_name = {item["name"]: item for item in model_items}
    assert "沙袋" in model_by_name
    assert model_by_name["铁丝网障碍"]["generation_mode_hint"] == TEXT_TO_3D_PREFERRED
    assert model_by_name["栅栏"]["generation_mode_hint"] == TEXT_TO_3D_PREFERRED
    assert by_name["海报"].target_pipeline == SUBSTRATE
    assert by_name["森林"].target_pipeline == SUBSTRATE
    assert by_name["天空"].target_pipeline == SUBSTRATE
    summary = summarize_classification(routed)
    assert "高风险薄片/网状对象：铁丝网障碍、栅栏" in summary


if __name__ == "__main__":
    test_substrate_terms_route_away_from_model_generation()
    print("[OK] substrate terms route away from model generation")
    test_layout_terms_are_disclosed_but_not_model_generated()
    print("[OK] layout terms are disclosed without model generation")
    test_thin_net_assets_avoid_image_to_3d_and_decals_route_to_substrate()
    print("[OK] thin/net assets avoid image-to-3D and decals route to substrate")
    print("\n=== SceneElementClassifier ALL PASS ===")
