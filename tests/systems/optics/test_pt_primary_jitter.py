"""PT's real virtual jitter selector preserves defaults and honors fixed rays."""
from _restir_cpu import ROOT, body, compile_run


def test_pt_primary_jitter_environment_and_virtual_dispatch():
    source = (ROOT / 'vision/src/render_core/integrator/pt.cpp').read_text(encoding='utf-8')
    token = '[[nodiscard]] bool jitter_primary_samples() const noexcept override'
    signature, implementation = body(source, token)
    assert 'std::getenv("VISION_PT_PRIMARY_JITTER")' in implementation
    compile_body = body(source, 'void compile() noexcept override')[1]
    assert 'primary_jitter={}' in compile_body
    assert 'jitter_primary_samples());' in compile_body
    method = (signature + implementation).replace('std::getenv', 'fixture_getenv')
    program = r'''
#include <cstring>
#include <iostream>
#include <stdexcept>
const char* fixture_value = nullptr;
const char* fixture_getenv(const char* key) {
    if (std::strcmp(key, "VISION_PT_PRIMARY_JITTER") != 0)
        throw std::runtime_error("Wrong environment key");
    return fixture_value;
}
struct Base {
    virtual bool jitter_primary_samples() const noexcept { return true; }
    virtual ~Base() = default;
};
struct PathTracing : Base {
    __METHOD__
};
int main() {
    PathTracing pt;
    Base* integrator = &pt;
    struct Case { const char* value; bool expected; };
    const Case cases[] = {{nullptr,true}, {"",true}, {"0",false}, {"00",false},
                          {"1",true}, {"2",true}, {"false",true}, {"-1",true}};
    for (const Case& item : cases) {
        fixture_value = item.value;
        if (integrator->jitter_primary_samples() != item.expected)
            throw std::runtime_error("PT jitter environment or virtual dispatch mismatch");
    }
    std::cout << "PASS PT production jitter selector: eight environment cases through base pointer\n";
}
'''.replace('__METHOD__', method)
    compile_run(program, 'pt-primary-jitter')


if __name__ == '__main__':
    test_pt_primary_jitter_environment_and_virtual_dispatch()
