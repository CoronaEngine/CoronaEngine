#include <corona/systems/script/python_lifecycle.h>

#include <iostream>

int main() {
    using Corona::Script::Python::PythonLifecycle;
    using Corona::Script::Python::PythonLifecycleState;

    PythonLifecycle lifecycle;
    if (lifecycle.state() != PythonLifecycleState::Created) {
        std::cerr << "initial state must be Created\n";
        return 1;
    }
    if (!lifecycle.transition(PythonLifecycleState::InterpreterInitializing) ||
        !lifecycle.transition(PythonLifecycleState::InterpreterReady) ||
        !lifecycle.transition(PythonLifecycleState::BackendInitializing) ||
        !lifecycle.transition(PythonLifecycleState::Running)) {
        std::cerr << "valid initialization transitions must succeed\n";
        return 1;
    }
    if (lifecycle.transition(PythonLifecycleState::InterpreterReady)) {
        std::cerr << "backward transition must fail\n";
        return 1;
    }
    if (!lifecycle.request_stop() || lifecycle.state() != PythonLifecycleState::StopRequested) {
        std::cerr << "request_stop must enter StopRequested\n";
        return 1;
    }
    if (!lifecycle.request_stop()) {
        std::cerr << "request_stop must be idempotent\n";
        return 1;
    }
    if (!lifecycle.transition(PythonLifecycleState::Stopping) ||
        !lifecycle.transition(PythonLifecycleState::Stopped)) {
        std::cerr << "shutdown transitions must succeed\n";
        return 1;
    }
    if (lifecycle.request_stop()) {
        std::cerr << "stopped lifecycle must not restart\n";
        return 1;
    }
    return 0;
}
