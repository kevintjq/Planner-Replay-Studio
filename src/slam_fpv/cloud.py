"""PointCloud2 decoding including organized row padding and endian conversion."""
import numpy as np

DTYPES = {1:'i1', 2:'u1', 3:'i2', 4:'u2', 5:'i4', 6:'u4', 7:'f4', 8:'f8'}


def decode(msg):
    endian = '>' if msg.is_bigendian else '<'
    fields = {f.name:f for f in msg.fields}
    if not all(k in fields for k in ('x', 'y', 'z')):
        raise ValueError("PointCloud2 must contain x,y,z")
    if msg.row_step < msg.width*msg.point_step or len(msg.data) < msg.row_step*msg.height:
        raise ValueError("Truncated/invalid PointCloud2 strides")
    def field(name):
        f = fields[name]
        if f.count != 1 or f.datatype not in DTYPES:
            raise ValueError(f"Unsupported PointField {name}")
        dtype = np.dtype(endian+DTYPES[f.datatype])
        if f.offset < 0 or f.offset+dtype.itemsize > msg.point_step:
            raise ValueError("PointField exceeds point_step")
        return np.ndarray((msg.height, msg.width), dtype=dtype, buffer=msg.data,
                          offset=f.offset, strides=(msg.row_step, msg.point_step)).reshape(-1)
    xyz = np.column_stack([field(k) for k in ('x','y','z')]).astype(float)
    rgb = None
    name = next((k for k in ('rgb', 'rgba') if k in fields), None)
    if name:
        packed = field(name)
        if packed.dtype.itemsize != 4:
            raise ValueError("Packed RGB must be float32/uint32/int32")
        bits = packed.view(np.dtype(endian+'u4')).astype(np.uint32)
        rgb = np.column_stack([(bits >> 16)&255, (bits >> 8)&255, bits&255]).astype('u1')
    mask = np.isfinite(xyz).all(axis=1)
    return xyz[mask], None if rgb is None else rgb[mask]


# Green at the low end, violet at the high end.
SCAN_COLORS = np.array([
    [30, 148, 47], [36, 234, 77], [26, 241, 155], [18, 214, 230],
    [25, 131, 255], [52, 61, 251], [114, 46, 235], [172, 79, 237],
], dtype=float)


def scan_colors(values, limits):
    low, high = limits
    scale = np.clip((values - low) / (high - low), 0., 1.) * (len(SCAN_COLORS) - 1)
    return np.column_stack([
        np.interp(scale, np.arange(len(SCAN_COLORS)), SCAN_COLORS[:, channel])
        for channel in range(3)
    ]).astype('u1')


class CloudMap:
    def __init__(self, voxel=.08, max_points=200000, *, scan_height=False, height_limits=None,
                 range_color=False, range_limits=None):
        self.voxel, self.max_points = voxel, max_points
        self.scan_height, self.height_limits = scan_height, height_limits
        self.range_color, self.range_limits = range_color, range_limits
        self.xyz = np.empty((0, 3))
        self.rgb = np.empty((0, 3), dtype='u1')

    def add(self, xyz, rgb=None, replace=False, *, origin=None):
        if self.range_color:
            if origin is not None and len(xyz):
                ranges = np.linalg.norm(xyz - origin, axis=1)
                if self.range_limits is None:
                    low, high = np.percentile(ranges, (2, 98))
                    self.range_limits = float(low), float(max(high, low + .5))
                rgb = scan_colors(ranges, self.range_limits)
            else:
                rgb = np.broadcast_to(np.array([222, 227, 232], dtype='u1'), xyz.shape)
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
            _, idx = np.unique(np.floor(xyz/self.voxel).astype('i8'), axis=0, return_index=True)
            idx = np.sort(idx)[-self.max_points:]
            xyz, rgb = xyz[idx], rgb[idx]
        self.xyz, self.rgb = xyz[-self.max_points:], rgb[-self.max_points:]
