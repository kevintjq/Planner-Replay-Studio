"""PointCloud2 decoding including organized row padding and endian conversion."""
import numpy as np

from . import _cloud

FIELD_SIZES = {1: 1, 2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 4, 8: 8}


def decode(msg):
    fields = {field.name: field for field in msg.fields}
    if not all(name in fields for name in ("x", "y", "z")):
        raise ValueError("PointCloud2 must contain x,y,z")

    def descriptor(name):
        field = fields[name]
        if field.count != 1 or field.datatype not in FIELD_SIZES:
            raise ValueError(f"Unsupported PointField {name}")
        if field.offset < 0 or field.offset + FIELD_SIZES[field.datatype] > msg.point_step:
            raise ValueError("PointField exceeds point_step")
        return field.offset, field.datatype

    name = next((name for name in ("rgb", "rgba") if name in fields), None)
    rgb = -1
    if name is not None:
        rgb, kind = descriptor(name)
        if FIELD_SIZES[kind] != 4:
            raise ValueError("Packed RGB must be float32/uint32/int32")
    points, colors, count = _cloud.decode_points(msg.data, msg.width, msg.height, msg.row_step,
                                                 msg.point_step, msg.is_bigendian,
                                                 *(descriptor(axis) for axis in ("x", "y", "z")), rgb)
    xyz = np.frombuffer(points, dtype=np.float64, count=count*3).reshape(-1, 3)
    return xyz, None if colors is None else np.frombuffer(colors, dtype=np.uint8, count=count*3).reshape(-1, 3)


# Green at the low end, violet at the high end.
SCAN_COLORS = np.array([
    [30, 148, 47], [36, 234, 77], [26, 241, 155], [18, 214, 230],
    [25, 131, 255], [52, 61, 251], [114, 46, 235], [172, 79, 237],
], dtype=np.uint8)


def scan_colors(values, limits):
    low, high = limits
    scale = np.clip((values - low) / (high - low), 0., 1.) * (len(SCAN_COLORS) - 1)
    return np.column_stack([
        np.interp(scale, np.arange(len(SCAN_COLORS)), SCAN_COLORS[:, channel])
        for channel in range(3)
    ]).astype('u1')


def first_voxel_indices(xyz, size):
    points = np.ascontiguousarray(xyz, dtype=np.float64)
    if points.ndim != 2 or points.shape[1] != 3:
        raise ValueError("Expected an array of XYZ points")
    return np.frombuffer(_cloud.first_voxel_indices(points, size), dtype=np.intp)


class CloudMap:
    def __init__(self, voxel=.08, max_points=200000, *, scan_height=False, height_limits=None,
                 range_color=False, range_limits=None):
        self.voxel, self.max_points = voxel, max_points
        self.scan_height, self.height_limits = scan_height, height_limits
        self.range_color, self.range_limits = range_color, range_limits
        self.xyz = np.empty((0, 3))
        self.rgb = np.empty((0, 3), dtype='u1')

    def add(self, xyz, rgb=None, replace=False):
        if self.range_color:
            rgb = np.zeros(xyz.shape, dtype='u1')
        elif self.scan_height:
            if self.height_limits is None and len(xyz):
                low, high = np.percentile(xyz[:, 2], (2, 98))
                self.height_limits = float(low), float(max(high, low + .5))
            rgb = scan_colors(xyz[:, 2], self.height_limits) if len(xyz) else np.empty((0, 3), dtype='u1')
        elif rgb is None:
            h = np.clip((xyz[:, 2]+1)/5, 0, 1)
            rgb = np.column_stack([50+180*h, 180-50*h, 230-150*h]).astype('u1')
        # Limit incoming work as well as the retained map.
        step = max(1, int(np.ceil(len(xyz)/self.max_points)))
        xyz, rgb = xyz[::step], rgb[::step]
        if not replace:
            xyz, rgb = np.concatenate([self.xyz, xyz]), np.concatenate([self.rgb, rgb])
        if self.voxel and len(xyz):
            idx = np.sort(first_voxel_indices(xyz, self.voxel))[-self.max_points:]
            xyz, rgb = xyz[idx], rgb[idx]
        self.xyz, self.rgb = xyz[-self.max_points:], rgb[-self.max_points:]

    def colors_from(self, position):
        """Color the retained map relative to the body at the current playback time."""
        if not len(self.xyz):
            return self.rgb
        if self.range_limits is None:
            offsets = self.xyz - position
            distances = np.sqrt(np.einsum("ij,ij->i", offsets, offsets))
            low, high = np.percentile(distances, (2, 98))
            self.range_limits = float(low), float(max(high, low + .5))
        points = np.ascontiguousarray(self.xyz, dtype=np.float64)
        colors = _cloud.range_colors(points, SCAN_COLORS, tuple(position), tuple(self.range_limits))
        return np.frombuffer(colors, dtype=np.uint8).reshape(-1, 3)
