#include <corona/systems/script/engine_scripts.h>
#include <nanobind/nanobind.h>

NB_MODULE(CoronaEngine, m) {
    m.doc() = "CoronaEngine embedded Python module (nanobind)";
    EngineScripts::BindAll(m);
    EngineScripts::BindEditorHost(m);
    EngineScripts::BindCef(m);
}
