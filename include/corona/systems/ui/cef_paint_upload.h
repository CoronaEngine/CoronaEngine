#pragma once

namespace Corona::Systems::UI {

/**
 * @brief 对一次 CEF 绘制（PET_VIEW）应当如何处理。
 *
 * 背景（实测）：CEF 在 resize 之后仍可能送达一帧**旧视口尺寸**的绘制。若把这种缓冲按**纹理**的
 * 行宽消费，每行起点都会错位 ⇒ 面板内容被斜切（实测错位 +32 / −22 / −81 px）。修复思路是
 * **推迟纹理重建**：resize 只更新"请求尺寸"，纹理一直保留到**尺寸匹配的绘制**到达时才重建，
 * 这样等待期间旧纹理继续显示（按新矩形拉伸），不会出现空纹理帧。
 */
enum class PaintUploadAction {
    /// 绘制尺寸与**当前纹理**一致 ⇒ 直接上传。行一致就安全，因此**即使请求尺寸已经变化也照传**
    /// （拖动中 CEF 晚一帧的绘制不该被丢掉，丢掉只会让内容更陈旧）。
    Upload,
    /// 绘制尺寸与**请求尺寸**一致、但与当前纹理不同 ⇒ 先重建纹理再上传（同一次 update 内完成，
    /// 内容不会缺席，因此不会出现"清成透明"的一帧）。
    RecreateThenUpload,
    /// 与纹理和请求**都不**一致 ⇒ 拒绝上传（按纹理行宽消费会斜切）并请求重绘。
    RefuseStalePaint,
};

/**
 * @brief 决定一次绘制该如何处理。纯函数，无副作用，便于单测。
 *
 * @param paint_w/paint_h       CEF 送来的绘制缓冲尺寸
 * @param texture_w/texture_h   当前纹理尺寸（由已上传图像决定）
 * @param requested_w/requested_h 最近一次 resize 请求的尺寸（BrowserTab::width/height）
 */
PaintUploadAction decide_paint_upload(int paint_w,
                                      int paint_h,
                                      int texture_w,
                                      int texture_h,
                                      int requested_w,
                                      int requested_h);

}  // namespace Corona::Systems::UI
