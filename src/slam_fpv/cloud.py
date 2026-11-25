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


class CloudMap:
    def __init__(self, voxel=.08, max_points=200000):
        self.voxel, self.max_points = voxel, max_points
        self.xyz = np.empty((0, 3))
        self.rgb = np.empty((0, 3), dtype='u1')

    def add(self, xyz, rgb=None, replace=False):
        if rgb is None:
            h = np.clip((xyz[:, 2]+1)/5, 0, 1)
            rgb = np.column_stack([50+180*h, 180-50*h, 230-150*h]).astype('u1')
        # Limit incoming work as well as the retained map.
        step = max(1, int(np.ceil(len(xyz)/self.max_points)))
        xyz, rgb = xyz[::step], rgb[::step]
        if not replace:
            xyz, rgb = np.concatenate([self.xyz, xyz]), np.concatenate([self.rgb, rgb])
        if len(xyz):
            _, idx = np.unique(np.floor(xyz/self.voxel).astype('i8'), axis=0, return_index=True)
            idx = np.sort(idx)[-self.max_points:]
            xyz, rgb = xyz[idx], rgb[idx]
        self.xyz, self.rgb = xyz, rgb
