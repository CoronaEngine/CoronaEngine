#pragma once

#include <Python.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <memory>

namespace Corona::Script::Python {

struct PythonValueDeleter {
    void operator()(PyObject* value) const { Py_XDECREF(value); }
};
using PythonValue = std::unique_ptr<PyObject, PythonValueDeleter>;

struct PythonValueRecursion {
    static inline thread_local unsigned depth = 0;
    bool entered = false;
    PythonValueRecursion() {
        // CPython's recursion limit alone exceeds the available C++ stack in
        // small-stack callers. Bound native conversion before entering it.
        if (depth >= 128) {
            PyErr_SetString(PyExc_RecursionError, "Native service values exceed the supported nesting depth");
        } else if (Py_EnterRecursiveCall(" while converting native service values") == 0) {
            ++depth;
            entered = true;
        }
    }
    ~PythonValueRecursion() { if (entered) { --depth; Py_LeaveRecursiveCall(); } }
};

// Called with the GIL held. The caller owns the returned reference. Callback
// argument tuples retain their existing shape; service arrays become lists.
inline PyObject* json_to_python(const nlohmann::json& value, bool tuple_arrays = false) {
    PythonValueRecursion recursion;
    if (!recursion.entered) return nullptr;
    if (value.is_null()) return Py_NewRef(Py_None);
    if (value.is_boolean()) return PyBool_FromLong(value.get<bool>());
    if (value.is_number_unsigned()) return PyLong_FromUnsignedLongLong(value.get<std::uint64_t>());
    if (value.is_number_integer()) return PyLong_FromLongLong(value.get<std::int64_t>());
    if (value.is_number_float()) return PyFloat_FromDouble(value.get<double>());
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        return PyUnicode_FromStringAndSize(text.data(), static_cast<Py_ssize_t>(text.size()));
    }
    if (value.is_array()) {
        PythonValue result(tuple_arrays ? PyTuple_New(static_cast<Py_ssize_t>(value.size()))
                                        : PyList_New(static_cast<Py_ssize_t>(value.size())));
        if (!result) return nullptr;
        for (std::size_t i = 0; i < value.size(); ++i) {
            auto* item = json_to_python(value[i], tuple_arrays);
            if (!item) return nullptr;
            if (tuple_arrays) PyTuple_SET_ITEM(result.get(), static_cast<Py_ssize_t>(i), item);
            else PyList_SET_ITEM(result.get(), static_cast<Py_ssize_t>(i), item);
        }
        return result.release();
    }
    if (value.is_object()) {
        PythonValue result(PyDict_New());
        if (!result) return nullptr;
        for (auto it = value.begin(); it != value.end(); ++it) {
            PythonValue key(PyUnicode_FromStringAndSize(it.key().data(), static_cast<Py_ssize_t>(it.key().size())));
            PythonValue item(json_to_python(it.value(), tuple_arrays));
            if (!key || !item || PyDict_SetItem(result.get(), key.get(), item.get()) < 0) return nullptr;
        }
        return result.release();
    }
    PyErr_SetString(PyExc_TypeError, "Unsupported native service value");
    return nullptr;
}

inline bool python_to_json(PyObject* value, nlohmann::json& result) {
    PythonValueRecursion recursion;
    if (!recursion.entered) return false;
    if (value == Py_None) { result = nullptr; return true; }
    if (PyBool_Check(value)) { result = value == Py_True; return true; }
    if (PyLong_Check(value)) {
        int overflow = 0;
        const auto number = PyLong_AsLongLongAndOverflow(value, &overflow);
        if (!overflow && !PyErr_Occurred()) { result = number; return true; }
        if (overflow > 0) {
            const auto positive = PyLong_AsUnsignedLongLong(value);
            if (!PyErr_Occurred()) { result = positive; return true; }
        }
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_OverflowError, "Service integer exceeds the native 64-bit range");
        return false;
    }
    if (PyFloat_Check(value)) {
        const auto number = PyFloat_AsDouble(value);
        if (!std::isfinite(number)) {
            PyErr_SetString(PyExc_ValueError, "Service number must be finite");
            return false;
        }
        result = number; return true;
    }
    if (PyUnicode_Check(value)) {
        Py_ssize_t size = 0;
        const auto* text = PyUnicode_AsUTF8AndSize(value, &size);
        if (!text) return false;
        result = std::string(text, static_cast<std::size_t>(size)); return true;
    }
    if (PyList_Check(value) || PyTuple_Check(value)) {
        result = nlohmann::json::array();
        const auto size = PyList_Check(value) ? PyList_GET_SIZE(value) : PyTuple_GET_SIZE(value);
        for (Py_ssize_t i = 0; i < size; ++i) {
            nlohmann::json item;
            if (!python_to_json(PyList_Check(value) ? PyList_GET_ITEM(value, i) : PyTuple_GET_ITEM(value, i), item)) return false;
            result.push_back(std::move(item));
        }
        return true;
    }
    if (PyDict_Check(value)) {
        result = nlohmann::json::object();
        Py_ssize_t position = 0;
        PyObject *key, *item;
        while (PyDict_Next(value, &position, &key, &item)) {
            if (!PyUnicode_Check(key)) {
                PyErr_SetString(PyExc_TypeError, "Service object keys must be strings"); return false;
            }
            Py_ssize_t size = 0;
            const auto* text = PyUnicode_AsUTF8AndSize(key, &size);
            if (!text) return false;
            nlohmann::json converted;
            if (!python_to_json(item, converted)) return false;
            result[std::string(text, static_cast<std::size_t>(size))] = std::move(converted);
        }
        return true;
    }
    PyErr_SetString(PyExc_TypeError, "Unsupported Python service result type");
    return false;
}

}  // namespace Corona::Script::Python
