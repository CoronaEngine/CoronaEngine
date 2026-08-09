#include <corona/systems/script/engine_scripts.h>
#include <nanobind/nanobind.h>

NB_MODULE(CoronaEngine, m) {
    m.doc() = "CoronaEngine embedded Python module (nanobind)";
    EngineScripts::BindAll(m);
    EngineScripts::BindEditorCompatibility(m);
    EngineScripts::BindEditorNetwork(m);
    EngineScripts::BindCef(m);
}
