#include <corona/systems/script/engine_scripts.h>
#include <nanobind/nanobind.h>

// Two groups, and no third: the engine object model that used to sit under `.script` went
// with the script runtime it served. What is left is what the embedded host calls and the
// transport the editor needs; `tests/tools/python_surface.py` pins both.
NB_MODULE(CoronaEngine, m) {
    m.doc() = "CoronaEngine embedded Python module (nanobind): the host-support entries the "
              "embedded host calls, plus the editor transport";
    EngineScripts::BindHostSupport(m);
    EngineScripts::BindCef(m);
}
