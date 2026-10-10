#include "dsl/dsl.h"
#include "rhi/context.h"
#include "dsl/data/registrable.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

void check_shader_comment_identity();

namespace {
using namespace ocarina;

void expect(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void check_uniform_capture(Device &device, Stream &stream) {
    EncodedData<uint> binding{17u};
    auto output = device.create_buffer<uint>(1, "binding parameter result");
    Kernel kernel = [&](BufferVar<uint> result) {
        result.write(0u, binding.as_parameter());
    };
    auto shader = device.compile(kernel, "binding_parameter_capture");
    uint actual{};
    stream << shader(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 17u, "captured uniform must reach the kernel parameter block");
    binding = 29u;
    stream << shader(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 29u, "dispatch must read the current binding, not a compile-time snapshot");
    auto other_output = device.create_buffer<uint>(1, "queued binding result");
    binding = 31u;
    auto first_dispatch = shader(output).dispatch(1u);
    binding = 47u;
    auto second_dispatch = shader(other_output).dispatch(1u);
    binding = 99u;
    uint other_actual{};
    stream << first_dispatch << second_dispatch << output.download(&actual)
           << other_output.download(&other_actual) << synchronize() << commit();
    expect(actual == 31u && other_actual == 47u,
           "queued dispatches must retain their own parameter snapshots");
    EncodedData<uint> moved{std::move(binding)};
    binding = 103u;
    moved = 61u;
    stream << shader(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 61u, "moving a parameter owner must preserve existing shader bindings");
    auto copied = moved;
    copied = 73u;
    expect(moved.hv() == 61u, "copying a parameter must keep values independent");
    EncodedData<uint> restored;
    restored = std::move(moved);
    moved = 79u;
    restored.hv() = 83u;
    stream << shader(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 83u, "restore-style move assignment and host reference writes must reach the shader");
    EncodedData<uint> replacement{97u};
    restored = replacement;
    replacement = 101u;
    stream << shader(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 97u, "copy assignment must update an existing captured owner without rebinding it");
    const std::function<uint()> getter = [] { return 123u; };
    bool rejected = false;
    try { restored = getter; } catch (const std::logic_error &) { rejected = true; }
    expect(rejected, "captured parameters must reject getter replacement in release builds too");
    EncodedData<uint> computed;
    computed = getter;
    rejected = false;
    try { restored = computed; } catch (const std::logic_error &) { rejected = true; }
    expect(rejected, "copy assignment must not convert a captured parameter into a getter");
    stream << shader(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 97u, "rejected getter replacement must preserve the live captured value");
    std::cout << "PASS: captured uniform refreshes at dispatch\n";
}

void check_registered_buffer_identity(Device &device, Stream &stream) {
    auto table = device.create_bindless_array();
    RegistrableBuffer<uint> first{table};
    RegistrableBuffer<uint> second{table};
    first.super() = device.create_buffer<uint>(1, "binding first");
    second.super() = device.create_buffer<uint>(1, "binding second");
    first.register_self();
    // This unused registration deliberately changes allocation history.
    auto padding = device.create_buffer<uint>(1, "binding padding");
    (void)table.emplace(padding);
    second.register_self();
    expect(first.index().hv() != second.index().hv(), "fixture must use different bindless indices");
    uint first_value = 13u, second_value = 71u;
    first.upload_immediately(&first_value);
    second.upload_immediately(&second_value);
    stream << table.update_slotSOA() << table.upload_handles() << synchronize() << commit();
    auto make_kernel = [](const RegistrableBuffer<uint> &input) {
        return Kernel{[&](BufferVar<uint> result) { result.write(0u, input.read(0u)); }};
    };
    auto a = make_kernel(first);
    auto b = make_kernel(second);
    expect(a.function()->hash() == b.function()->hash(),
           "same shader must have the same identity when bindless allocation history changes");
    auto shader_a = device.compile(a, "registered_binding_cache");
    auto shader_b = device.compile(b, "registered_binding_cache");
    auto output = device.create_buffer<uint>(1, "registered binding result");
    uint actual{};
    stream << shader_a(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 13u, "first cached shader must use its own binding");
    stream << shader_b(output).dispatch(1u) << output.download(&actual) << synchronize() << commit();
    expect(actual == 71u, "cache reuse must bind the second buffer rather than the first one");
    std::cout << "PASS: registered buffers share shader identity and retain independent bindings\n";
}
}

int main(int argc, char **argv) {
    try {
        Env::set_valid_check(false);
        if (argc != 2 || std::string_view{argv[1]} != "--bindings-only") check_shader_comment_identity();
        if (argc == 2 && std::string_view{argv[1]} == "--comments-only") return 0;
        DynamicModule::clear_search_path();
        RHIContext::instance().init(std::filesystem::current_path());
        auto device = RHIContext::instance().create_device("cuda");
        auto stream = device.create_stream();
        check_uniform_capture(device, stream);
        check_registered_buffer_identity(device, stream);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
