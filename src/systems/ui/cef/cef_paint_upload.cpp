#include <corona/systems/ui/cef_paint_upload.h>

namespace Corona::Systems::UI {

PaintUploadAction decide_paint_upload(int paint_w,
                                      int paint_h,
                                      int texture_w,
                                      int texture_h,
                                      int requested_w,
                                      int requested_h) {
    if (paint_w <= 0 || paint_h <= 0) {
        return PaintUploadAction::RefuseStalePaint;
    }

    // Always safe: the buffer is row-consistent with the texture we would draw with. Showing it
    // costs nothing and keeps the panel content fresh while a resize is still being applied, so
    // this must be checked BEFORE the request comparison - a paint that matches the texture but
    // not the latest request is usable content, not stale content.
    if (paint_w == texture_w && paint_h == texture_h) {
        return PaintUploadAction::Upload;
    }

    // The texture no longer matches this paint. Only rebuild it when the paint is exactly what we
    // asked CEF for; anything else would be consumed at the wrong row length and shear.
    if (requested_w > 0 && requested_h > 0 && paint_w == requested_w && paint_h == requested_h) {
        return PaintUploadAction::RecreateThenUpload;
    }

    return PaintUploadAction::RefuseStalePaint;
}

}  // namespace Corona::Systems::UI
