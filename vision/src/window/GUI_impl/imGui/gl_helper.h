//
// Created by GitHub Copilot on 2026/4/9.
//

#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "core/util/logging.h"

namespace vision::gui_gl_detail {
[[nodiscard]] inline auto gl_error_string(GLenum error) noexcept {
    OC_USING_SV;
    switch (error) {
        case GL_INVALID_ENUM: return "invalid enum"sv;
        case GL_INVALID_VALUE: return "invalid value"sv;
        case GL_INVALID_OPERATION: return "invalid operation"sv;
        case GL_OUT_OF_MEMORY: return "out of memory"sv;
        default: return "unknown error"sv;
    }
}
}// namespace vision::gui_gl_detail

#define VISION_CHECK_GL(...)                                   \
    [&] {                                                      \
        __VA_ARGS__;                                           \
        if (auto error = glGetError(); error != GL_NO_ERROR) { \
            OC_ERROR_FORMAT(                                   \
                "OpenGL error: {}.",                          \
                ::vision::gui_gl_detail::gl_error_string(error));     \
        }                                                      \
    }()

namespace vision::gui_gl_detail {

inline GLuint create_gl_shader(const std::string &source, GLuint shader_type) {
    GLuint shader = glCreateShader(shader_type);
    const GLchar *source_data = reinterpret_cast<const GLchar *>(source.data());
    glShaderSource(shader, 1, &source_data, nullptr);
    glCompileShader(shader);

    GLint is_compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);
    if (is_compiled == GL_FALSE) {
        GLint max_length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &max_length);

        std::string info_log(max_length, '\0');
        GLchar *info_log_data = reinterpret_cast<GLchar *>(&info_log[0]);
        glGetShaderInfoLog(shader, max_length, nullptr, info_log_data);

        glDeleteShader(shader);
        std::cerr << "Compilation of shader failed: " << info_log << std::endl;
        return 0;
    }
    return shader;
}

inline GLuint create_gl_program(const std::string &vert_source, const std::string &frag_source) {
    GLuint vert_shader = create_gl_shader(vert_source, GL_VERTEX_SHADER);
    if (vert_shader == 0) {
        return 0;
    }

    GLuint frag_shader = create_gl_shader(frag_source, GL_FRAGMENT_SHADER);
    if (frag_shader == 0) {
        glDeleteShader(vert_shader);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vert_shader);
    glAttachShader(program, frag_shader);
    glLinkProgram(program);

    GLint is_linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &is_linked);
    if (is_linked == GL_FALSE) {
        GLint max_length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &max_length);

        std::string info_log(max_length, '\0');
        GLchar *info_log_data = reinterpret_cast<GLchar *>(&info_log[0]);
        glGetProgramInfoLog(program, max_length, nullptr, info_log_data);

        std::cerr << "Linking of program failed: " << info_log << std::endl;
        glDeleteProgram(program);
        glDeleteShader(vert_shader);
        glDeleteShader(frag_shader);
        return 0;
    }

    glDetachShader(program, vert_shader);
    glDetachShader(program, frag_shader);
    return program;
}

}// namespace vision::gui_gl_detail