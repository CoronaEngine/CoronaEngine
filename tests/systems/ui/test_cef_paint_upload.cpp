// CEF 绘制缓冲上传决策的单测。
//
// 背景（实测）：`update_texture()` 曾用纹理宽高算期望字节数，缓冲更大就按纹理行宽直接消费。
// 而 CEF 可能有一帧**旧视口尺寸**的绘制在 resize 之后才到达，此时缓冲行宽 ≠ 纹理宽
// ⇒ 每行起点错位 ⇒ 面板内容斜切（实测错位 +32 / −22 / −81 px，93 次缩放期间 mismatch ≥120 次）。
//
// 修复的方向是**推迟纹理重建**：resize 只更新请求尺寸，纹理等"尺寸匹配的绘制"到达时才重建，
// 这样旧纹理在等待期间继续显示（不会出现空纹理帧）。本文件把该决策钉死。

#include <corona/systems/ui/cef_paint_upload.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (condition) {
        return;
    }
    std::cerr << "CefPaintUploadTests failed: " << message << '\n';
    std::exit(1);
}

using Corona::Systems::UI::PaintUploadAction;
using Corona::Systems::UI::decide_paint_upload;

void test_matching_paint_into_matching_texture_uploads() {
    require(decide_paint_upload(1480, 790, 1480, 790, 1480, 790) == PaintUploadAction::Upload,
            "a paint that matches both the request and the texture must upload as-is");
}

void test_matching_paint_recreates_a_stale_texture() {
    // The panel was resized (request 1448x790) while the texture is still the previous 1480x790.
    // The first paint at the new size must rebuild the texture and then upload, so the content is
    // never rendered from an empty texture.
    require(decide_paint_upload(1448, 790, 1480, 790, 1448, 790) ==
                PaintUploadAction::RecreateThenUpload,
            "the first paint at the requested size must recreate the texture before uploading");
}

void test_paint_for_the_previous_size_is_refused() {
    // A paint issued before the resize arrives after it: uploading it at the texture's row length
    // would shear every row.
    require(decide_paint_upload(1480, 790, 1448, 790, 1448, 790) == PaintUploadAction::RefuseStalePaint,
            "a paint for the previous size must be refused, not sheared");
    require(decide_paint_upload(631, 790, 653, 790, 653, 790) == PaintUploadAction::RefuseStalePaint,
            "a smaller stale paint must be refused too");
}

void test_refuses_while_the_request_is_still_the_texture_size() {
    // Idle: request == texture, so any paint that matches neither is stale.
    require(decide_paint_upload(1300, 700, 1480, 790, 1480, 790) == PaintUploadAction::RefuseStalePaint,
            "a paint that matches neither the texture nor the request must be refused");
}

void test_paint_matching_the_texture_is_used_even_if_the_request_moved_on() {
    // Mid-drag: the request already moved to 1446x790 while the texture is still 1480x790, and CEF
    // answers with one more paint at the old size. That buffer is row-consistent with the texture
    // we would draw with, so uploading it is safe and keeps the content fresh; rebuilding the
    // texture for it is not (it is not what we asked for).
    require(decide_paint_upload(1480, 790, 1480, 790, 1446, 790) == PaintUploadAction::Upload,
            "a row-consistent paint must be used even when the request has moved on");
}

void test_unset_request_still_allows_a_texture_consistent_paint() {
    // No request yet (fresh tab): a paint that matches the texture is still safe to upload; it must
    // simply never authorise a rebuild.
    require(decide_paint_upload(1480, 790, 1480, 790, 0, 0) == PaintUploadAction::Upload,
            "an unset request must not block a paint that matches the texture");
    require(decide_paint_upload(1446, 790, 1480, 790, 0, 0) != PaintUploadAction::RecreateThenUpload,
            "an unset request must never authorise a texture rebuild");
}

void test_only_one_axis_mismatching_is_still_stale() {
    require(decide_paint_upload(1480, 800, 1480, 790, 1480, 790) == PaintUploadAction::RefuseStalePaint,
            "a height-only mismatch is still a stale paint");
    require(decide_paint_upload(1480, 800, 1480, 790, 1480, 800) == PaintUploadAction::RecreateThenUpload,
            "a height-only match against the request must recreate the texture");
}

void test_invalid_sizes_are_refused() {
    require(decide_paint_upload(0, 0, 1480, 790, 1480, 790) == PaintUploadAction::RefuseStalePaint,
            "a paint with no pixels must be refused");
    require(decide_paint_upload(-1, 790, 1480, 790, 1480, 790) == PaintUploadAction::RefuseStalePaint,
            "a negative paint size must be refused");
}

}  // namespace

int main() {
    test_matching_paint_into_matching_texture_uploads();
    test_matching_paint_recreates_a_stale_texture();
    test_paint_for_the_previous_size_is_refused();
    test_refuses_while_the_request_is_still_the_texture_size();
    test_paint_matching_the_texture_is_used_even_if_the_request_moved_on();
    test_unset_request_still_allows_a_texture_consistent_paint();
    test_only_one_axis_mismatching_is_still_stale();
    test_invalid_sizes_are_refused();
    std::cout << "CefPaintUploadTests passed\n";
    return 0;
}
