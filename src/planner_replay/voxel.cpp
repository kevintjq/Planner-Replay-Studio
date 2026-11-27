#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
using Key = std::array<std::int64_t, 3>;
constexpr auto row_bytes = static_cast<Py_ssize_t>(3 * sizeof(double));

struct Voxel {
    Key key;
    Py_ssize_t index;
};

std::int64_t cell(double coordinate, double size) {
    const double value = std::floor(coordinate / size);
    constexpr double limit = 9223372036854775808.0;
    if (!(value >= -limit && value < limit)) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

std::vector<Py_ssize_t> select_first(const char* points, Py_ssize_t count, double size) {
    std::vector<Voxel> voxels;
    voxels.reserve(count);
    for (Py_ssize_t i = 0; i < count; ++i) {
        double point[3];
        std::memcpy(point, points + i * row_bytes, row_bytes);
        voxels.push_back({{cell(point[0], size), cell(point[1], size), cell(point[2], size)}, i});
    }
    std::sort(voxels.begin(), voxels.end(), [](const Voxel& a, const Voxel& b) {
        return a.key == b.key ? a.index < b.index : a.key < b.key;
    });

    std::vector<Py_ssize_t> result;
    result.reserve(voxels.size());
    for (std::size_t i = 0; i < voxels.size(); ++i) {
        if (i == 0 || voxels[i].key != voxels[i - 1].key) {
            result.push_back(voxels[i].index);
        }
    }
    return result;
}

PyObject* first_voxel_indices(PyObject*, PyObject* args) {
    Py_buffer points{};
    double size;
    if (!PyArg_ParseTuple(args, "y*d", &points, &size)) {
        return nullptr;
    }
    if (points.len % row_bytes != 0 || !std::isfinite(size) || size <= 0) {
        PyBuffer_Release(&points);
        PyErr_SetString(PyExc_ValueError, "Expected packed XYZ doubles and a positive finite voxel size");
        return nullptr;
    }

    std::vector<Py_ssize_t> indices;
    bool out_of_memory = false;
    Py_BEGIN_ALLOW_THREADS
    try {
        indices = select_first(static_cast<const char*>(points.buf), points.len / row_bytes, size);
    } catch (const std::bad_alloc&) {
        out_of_memory = true;
    }
    Py_END_ALLOW_THREADS
    PyBuffer_Release(&points);
    if (out_of_memory) {
        return PyErr_NoMemory();
    }
    return PyBytes_FromStringAndSize(reinterpret_cast<const char*>(indices.data()),
                                     static_cast<Py_ssize_t>(indices.size() * sizeof(Py_ssize_t)));
}

PyMethodDef methods[] = {
    {"first_voxel_indices", first_voxel_indices, METH_VARARGS, "Return packed indices in voxel order."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_voxel", "Point-cloud voxel downsampling.", -1, methods
};
}

PyMODINIT_FUNC PyInit__voxel() {
    return PyModule_Create(&module);
}
