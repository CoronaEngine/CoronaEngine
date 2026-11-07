#include "python_value.h"
#include <iostream>
#include <limits>

using namespace Corona::Script::Python;

int main() {
    Py_Initialize();
    bool ok = true;
    const auto require = [&](bool value, const char* message) {
        if (!value) { std::cerr << message << '\n'; ok = false; }
    };
    {
        const nlohmann::json source = {
            {"id", std::numeric_limits<std::uint64_t>::max()},
            {std::string("key\0suffix", 10), std::string("text\0suffix", 11)},
            {"values", {true, false, nullptr, -42, 1.25, "Unicode: 中文"}},
        };
        PythonValue value(json_to_python(source));
        require(value && PyDict_Check(value.get()), "structured object conversion failed");
        nlohmann::json result;
        require(value && python_to_json(value.get(), result) && result == source,
                "owned values lost integer precision, NUL text, keys or nested data");
        PythonValue list(json_to_python(nlohmann::json::array({1, 2})));
        PythonValue tuple(json_to_python(nlohmann::json::array({1, 2}), true));
        require(list && PyList_Check(list.get()), "service arrays must remain mutable Python lists");
        require(tuple && PyTuple_Check(tuple.get()), "callback arguments must retain their tuple contract");
        PythonValue cycle(PyList_New(0));
        PyList_Append(cycle.get(), cycle.get());
        require(!python_to_json(cycle.get(), result) && PyErr_ExceptionMatches(PyExc_RecursionError),
                "cyclic Python results must fail explicitly");
        PyErr_Clear();
        PyList_SetSlice(cycle.get(), 0, 1, nullptr);
        PythonValue oversized(PyLong_FromString(const_cast<char*>("18446744073709551616"), nullptr, 10));
        require(!python_to_json(oversized.get(), result) && PyErr_ExceptionMatches(PyExc_OverflowError),
                "integers outside native range must not silently round");
        PyErr_Clear();
        PythonValue nonfinite(PyFloat_FromDouble(std::numeric_limits<double>::infinity()));
        require(!python_to_json(nonfinite.get(), result) && PyErr_ExceptionMatches(PyExc_ValueError),
                "nonfinite results must not silently become null");
        PyErr_Clear();
        PythonValue unsupported(PySet_New(nullptr));
        require(!python_to_json(unsupported.get(), result) && PyErr_ExceptionMatches(PyExc_TypeError),
                "unsupported Python objects must not become strings");
        PyErr_Clear();
    }
    Py_Finalize();
    return ok ? 0 : 1;
}
