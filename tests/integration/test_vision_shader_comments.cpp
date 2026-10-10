#include "dsl/dsl.h"
#include "generator/ast_to_cpp_source.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace ocarina;

void expect_comment_identity(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

auto make_comment_kernel(uint comment_count, const string &location, uint increment = 1u) {
    return Kernel{[&](BufferVar<uint> output, Uint input) {
        for (uint i = 0; i < comment_count; ++i) comment(location);
        if_(input > 0u, [&] {
            for (uint i = 0; i < comment_count; ++i) comment("branch: " + location);
            output.write(0u, input + increment);
        }).else_([&] {
            // A comment-only else must remain equivalent to an empty else.
            for (uint i = 0; i < comment_count; ++i) comment("empty else: " + location);
        });
        if_(input > 2u, [&] {
            output.write(0u, input + increment);
        }).else_([&] {
            // Comments surrounding an else-if must not change its emitted shape.
            for (uint i = 0; i < comment_count; ++i) comment("else if: " + location);
            if_(input == 0u, [&] { output.write(0u, increment); });
            for (uint i = 0; i < comment_count; ++i) comment("after else if: " + location);
        });
    }};
}

string source_for(const Function &function, bool obfuscation, bool emit_comments) {
    AstToCppSource emitter{obfuscation, emit_comments};
    emitter.emit(function);
    return emitter.scratch().c_str();
}
}// namespace

void check_shader_comment_identity() {
    const auto plain = make_comment_kernel(0u, "");
    const auto annotated = make_comment_kernel(1u, "E:/checkout/a.cpp,12");
    const auto relocated = make_comment_kernel(3u, "D:/other/checkout/b.cpp,937");
    const auto changed_code = make_comment_kernel(1u, "E:/checkout/a.cpp,12", 2u);
    const auto plain_hash = plain.function()->hash();
    expect_comment_identity(plain_hash == annotated.function()->hash(),
                            "adding debug comments must not change shader identity");
    expect_comment_identity(plain_hash == relocated.function()->hash(),
                            "comment count, source path and line changes must not change shader identity");
    expect_comment_identity(plain_hash != changed_code.function()->hash(),
                            "real shader instruction changes must invalidate shader identity");

    for (const bool obfuscation : {false, true}) {
        const auto source_plain = make_comment_kernel(0u, "");
        const auto source_annotated = make_comment_kernel(1u, "E:/checkout/a.cpp,12");
        const auto source_relocated = make_comment_kernel(3u, "D:/other/checkout/b.cpp,937");
        const auto source_changed = make_comment_kernel(1u, "E:/checkout/a.cpp,12", 2u);
        const auto plain_source = source_for(*source_plain.function(), obfuscation, false);
        expect_comment_identity(plain_source == source_for(*source_annotated.function(), obfuscation, false),
                                "omitting comments must also omit their statement separators and empty branches");
        expect_comment_identity(plain_source == source_for(*source_relocated.function(), obfuscation, false),
                                "canonical generated source must ignore relocated or repeated debug comments");
        expect_comment_identity(plain_source != source_for(*source_changed.function(), obfuscation, false),
                                "canonical generated source must retain instruction changes");
    }

    const auto diagnostic = make_comment_kernel(1u, "diagnostic.cpp,42");
    expect_comment_identity(source_for(*diagnostic.function(), false, true).find("diagnostic.cpp,42") != string::npos,
                            "diagnostic source generation must retain source comments");
    std::cout << "PASS: shader comments preserve identity and canonical source\n";
}
