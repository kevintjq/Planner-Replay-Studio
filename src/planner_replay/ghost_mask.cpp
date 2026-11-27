#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <algorithm>
#include <cstdlib>

namespace {
PyObject* rgb_difference(PyObject*, PyObject* args) {
    Py_buffer frame{}, first{}, second{}, third{};
    if (!PyArg_ParseTuple(args, "y*y*|y*y*", &frame, &first, &second, &third)) {
        return nullptr;
    }

    PyObject* output = nullptr;
    if (frame.len != first.len || frame.len % 3 != 0 ||
        ((second.buf || third.buf) && (!second.buf || !third.buf ||
                                      second.len != frame.len || third.len != frame.len))) {
        PyErr_SetString(PyExc_ValueError, "Expected matching RGB24 crops and one or three references");
    } else {
        output = PyBytes_FromStringAndSize(nullptr, frame.len / 3);
        if (output) {
            const auto* rgb = static_cast<const unsigned char*>(frame.buf);
            const auto* a = static_cast<const unsigned char*>(first.buf);
            const auto* b = static_cast<const unsigned char*>(second.buf);
            const auto* c = static_cast<const unsigned char*>(third.buf);
            auto* result = reinterpret_cast<unsigned char*>(PyBytes_AS_STRING(output));
            Py_BEGIN_ALLOW_THREADS
            for (Py_ssize_t offset = 0; offset < frame.len; offset += 3) {
                int peak = 0;
                for (int channel = 0; channel < 3; ++channel) {
                    const auto i = offset + channel;
                    int background = a[i];
                    if (b) {
                        background = std::max(std::min(a[i], b[i]), std::min(std::max(a[i], b[i]), c[i]));
                    }
                    peak = std::max(peak, std::abs(static_cast<int>(rgb[i]) - background));
                }
                result[offset / 3] = static_cast<unsigned char>(peak);
            }
            Py_END_ALLOW_THREADS
        }
    }
    for (Py_buffer* buffer : {&frame, &first, &second, &third}) {
        if (buffer->obj) {
            PyBuffer_Release(buffer);
        }
    }
    return output;
}
PyMethodDef methods[] = {
    {"rgb_difference", rgb_difference, METH_VARARGS, "Maximum per-channel difference from the reference median."},
    {nullptr, nullptr, 0, nullptr}
};
PyModuleDef module = {PyModuleDef_HEAD_INIT, "_ghost_mask", "Fixed-camera sprite mask operations.", -1, methods};
}

PyMODINIT_FUNC PyInit__ghost_mask() {
    return PyModule_Create(&module);
}
