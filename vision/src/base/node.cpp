//
// Created by Zero on 16/01/2023.
//

#include "mgr/pipeline.h"
#include "node.h"
#include "mgr/global.h"
#include "window/GUI/widgets.h"

namespace vision {

Pipeline *Node::pipeline() noexcept {
    return Global::instance().pipeline();
}

Scene &Node::scene() noexcept {
    return pipeline()->scene();
}

Renderer &Node::renderer() noexcept {
    return pipeline()->renderer();
}

fs::path Node::scene_path() noexcept {
    return Global::instance().scene_path();
}

TSpectrum &Node::spectrum() noexcept {
    return renderer().spectrum();
}

FrameBuffer &Node::frame_buffer() noexcept {
    return *pipeline()->frame_buffer();
}

Device &Node::device() noexcept {
    return pipeline()->device();
}

}// namespace vision