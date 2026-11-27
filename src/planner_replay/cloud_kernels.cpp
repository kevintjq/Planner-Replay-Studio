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

struct Field {
    Py_ssize_t offset;
    int type;
};

constexpr std::array<int, 9> field_sizes{0, 1, 1, 2, 2, 4, 4, 4, 8};

template <typename T>
T read_scalar(const char* address, bool swap) {
    T value;
    if (swap) {
        auto* bytes = reinterpret_cast<char*>(&value);
        std::reverse_copy(address, address + sizeof(T), bytes);
    } else {
        std::memcpy(&value, address, sizeof(T));
    }
    return value;
}

double read_coordinate(const char* address, Field field, bool swap) {
    const auto* value = address + field.offset;
    switch (field.type) {
    case 1: return read_scalar<std::int8_t>(value, false);
    case 2: return read_scalar<std::uint8_t>(value, false);
    case 3: return read_scalar<std::int16_t>(value, swap);
    case 4: return read_scalar<std::uint16_t>(value, swap);
    case 5: return read_scalar<std::int32_t>(value, swap);
    case 6: return read_scalar<std::uint32_t>(value, swap);
    case 7: return read_scalar<float>(value, swap);
    case 8: return read_scalar<double>(value, swap);
    }
    return 0.0;
}

bool fits_field(Py_ssize_t step, Field field) {
    return field.type > 0 && field.type < static_cast<int>(field_sizes.size()) &&
           field.offset >= 0 && field.offset <= step - field_sizes[field.type];
}

PyObject* decode_points(PyObject*, PyObject* args) {
    Py_buffer data{};
    Py_ssize_t width, height, row_step, point_step, rgb_offset;
    int big_endian;
    Field x{}, y{}, z{};
    if (!PyArg_ParseTuple(args, "y*nnnnp(ni)(ni)(ni)n", &data, &width, &height, &row_step,
                          &point_step, &big_endian, &x.offset, &x.type, &y.offset, &y.type,
                          &z.offset, &z.type, &rgb_offset)) {
        return nullptr;
    }
    const bool has_rgb = rgb_offset >= 0;
    if (width < 0 || height < 0 || row_step < 0 || point_step <= 0 ||
        width > row_step / point_step || (height && row_step > data.len / height) ||
        !fits_field(point_step, x) || !fits_field(point_step, y) || !fits_field(point_step, z) ||
        (has_rgb && !fits_field(point_step, {rgb_offset, 6}))) {
        PyBuffer_Release(&data);
        PyErr_SetString(PyExc_ValueError, "Invalid PointCloud2 layout");
        return nullptr;
    }

    // Valid row strides also bound width × height by the input buffer length.
    const Py_ssize_t count = width * height;
    if (count > PY_SSIZE_T_MAX / row_bytes) {
        PyBuffer_Release(&data);
        return PyErr_NoMemory();
    }
    PyObject* xyz = PyByteArray_FromStringAndSize(nullptr, count * row_bytes);
    PyObject* rgb = has_rgb ? PyByteArray_FromStringAndSize(nullptr, count * 3) : Py_NewRef(Py_None);
    if (!xyz || !rgb) {
        Py_XDECREF(xyz);
        Py_XDECREF(rgb);
        PyBuffer_Release(&data);
        return nullptr;
    }
    auto* coordinates = PyByteArray_AS_STRING(xyz);
    auto* colors = has_rgb ? reinterpret_cast<unsigned char*>(PyByteArray_AS_STRING(rgb)) : nullptr;
    const auto* input = static_cast<const char*>(data.buf);
    const std::uint16_t one = 1;
    const bool host_is_big_endian = *reinterpret_cast<const unsigned char*>(&one) == 0;
    const bool swap = bool(big_endian) != host_is_big_endian;
    Py_ssize_t used = 0;
    Py_BEGIN_ALLOW_THREADS
    for (Py_ssize_t row = 0; row < height && width; ++row) {
        const auto* address = input + row * row_step;
        for (Py_ssize_t column = 0; column < width; ++column, address += point_step) {
            const double point[]{read_coordinate(address, x, swap), read_coordinate(address, y, swap),
                                 read_coordinate(address, z, swap)};
            if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])) {
                continue;
            }
            std::memcpy(coordinates + used * row_bytes, point, row_bytes);
            if (colors) {
                const auto packed = read_scalar<std::uint32_t>(address + rgb_offset, swap);
                colors[used * 3] = static_cast<unsigned char>(packed >> 16);
                colors[used * 3 + 1] = static_cast<unsigned char>(packed >> 8);
                colors[used * 3 + 2] = static_cast<unsigned char>(packed);
            }
            ++used;
        }
    }
    Py_END_ALLOW_THREADS
    PyBuffer_Release(&data);
    if (used != count && (PyByteArray_Resize(xyz, used * row_bytes) < 0 ||
                          (has_rgb && PyByteArray_Resize(rgb, used * 3) < 0))) {
        Py_DECREF(xyz);
        Py_DECREF(rgb);
        return nullptr;
    }
    return Py_BuildValue("NNn", xyz, rgb, used);
}

PyObject* range_colors(PyObject*, PyObject* args) {
    Py_buffer points{}, palette{};
    double x, y, z, low, high;
    if (!PyArg_ParseTuple(args, "y*y*(ddd)(dd)", &points, &palette, &x, &y, &z, &low, &high)) {
        return nullptr;
    }
    if (points.len % row_bytes || palette.len < 6 || palette.len % 3 ||
        !std::isfinite(low) || !std::isfinite(high) || high <= low) {
        PyBuffer_Release(&points);
        PyBuffer_Release(&palette);
        PyErr_SetString(PyExc_ValueError, "Invalid range color inputs");
        return nullptr;
    }
    const auto count = points.len / row_bytes;
    auto* output = PyBytes_FromStringAndSize(nullptr, count * 3);
    if (output) {
        auto* colors = reinterpret_cast<unsigned char*>(PyBytes_AS_STRING(output));
        auto* rgb = static_cast<const unsigned char*>(palette.buf);
        const auto* xyz = static_cast<const char*>(points.buf);
        const auto last = palette.len / 3 - 1;
        const auto span = high - low;
        Py_BEGIN_ALLOW_THREADS
        for (Py_ssize_t i = 0; i < count; ++i) {
            double point[3];
            std::memcpy(point, xyz + i * row_bytes, row_bytes);
            const double dx = point[0] - x, dy = point[1] - y, dz = point[2] - z;
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double hue = std::min(1., std::max(0., (distance - low) / span)) * last;
            const auto index = std::min(static_cast<Py_ssize_t>(hue), last - 1);
            const double fraction = hue - index;
            for (int channel = 0; channel < 3; ++channel) {
                const int left = rgb[index * 3 + channel], right = rgb[(index + 1) * 3 + channel];
                colors[i * 3 + channel] = static_cast<unsigned char>((right - left) * fraction + left);
            }
        }
        Py_END_ALLOW_THREADS
    }
    PyBuffer_Release(&points);
    PyBuffer_Release(&palette);
    return output;
}

PyMethodDef methods[] = {
    {"first_voxel_indices", first_voxel_indices, METH_VARARGS, "Return packed indices in voxel order."},
    {"decode_points", decode_points, METH_VARARGS, "Unpack finite XYZ and optional RGB points."},
    {"range_colors", range_colors, METH_VARARGS, "Color packed world coordinates relative to a body position."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef module = {
    PyModuleDef_HEAD_INIT, "_cloud", "Point-cloud decoding, coloring, and voxel downsampling.", -1, methods
};
}

PyMODINIT_FUNC PyInit__cloud() {
    return PyModule_Create(&module);
}
