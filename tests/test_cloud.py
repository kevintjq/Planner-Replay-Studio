from types import SimpleNamespace as NS
import struct
import numpy as np
import pytest
from planner_replay.cloud import decode,CloudMap


@pytest.mark.parametrize('endian',['<','>'])
def test_organized_padding_rgb_nan(endian):
    fields=[NS(name=n,offset=o,datatype=d,count=1) for n,o,d in [('x',0,7),('y',4,7),('z',8,7),('rgb',12,7)]]
    data=b''.join(struct.pack(endian+'fffI',x,2,3,0xff8040)+b'padding!' for x in [1,float('nan')])
    msg=NS(width=1,height=2,point_step=16,row_step=24,fields=fields,data=data,is_bigendian=endian=='>')
    xyz,rgb=decode(msg)
    np.testing.assert_equal(xyz,[[1,2,3]])
    np.testing.assert_equal(rgb,[[255,128,64]])
    msg.data=data[:10]
    with pytest.raises(ValueError): decode(msg)


def test_map_voxel_bound_replace():
    m=CloudMap(.1,10)
    xyz=np.column_stack([np.arange(100),np.zeros(100),np.zeros(100)])
    m.add(xyz); assert len(m.xyz)<=10
    m.add(np.zeros((100,3)),replace=True)
    assert len(m.xyz)==1


def test_scan_palette_is_stable_and_zero_voxel_keeps_raw_samples():
    from planner_replay.cloud import CloudMap

    cloud = CloudMap(0, 5, scan_height=True, height_limits=(-1, 3))
    points = np.array([[0, 0, 0], [.001, 0, 0], [0, 0, 2]], dtype=float)
    cloud.add(points)
    first_colors = cloud.rgb.copy()
    assert len(cloud.xyz) == 3
    assert (first_colors[0] != first_colors[2]).any()
    cloud.add(points)
    assert len(cloud.xyz) == 5
    np.testing.assert_array_equal(cloud.rgb[-3:], first_colors)


def test_range_palette_recolors_existing_points_from_current_body_position():
    cloud = CloudMap(0, range_color=True, range_limits=(0., 10.))
    points = np.array([[8., 0., 0.], [4., 0., 4.]])
    cloud.add(points)
    first_colors = cloud.colors_from([4., 0., 0.])
    np.testing.assert_array_equal(first_colors[0], first_colors[1])
    new_colors = cloud.colors_from([8., 0., 0.])
    np.testing.assert_array_equal(cloud.xyz, points)
    assert not np.array_equal(new_colors[0], new_colors[1])
    assert not np.array_equal(new_colors[0], first_colors[0])


@pytest.mark.parametrize("extent", [7., 2**22])
def test_voxel_keys_preserve_the_first_point_across_signed_cells(extent):
    from planner_replay.cloud import first_voxel_indices

    points = np.array([[-extent, 1., 2.], [-extent + .25, 1., 2.],
                       [.25, -1.1, 2.], [.75, -.9, 2.], [.5, -.8, 2.],
                       [0., 0., 0.]])
    reference = np.unique(np.floor(points).astype(np.int64), axis=0, return_index=True)[1]
    np.testing.assert_array_equal(first_voxel_indices(points, 1.), reference)


def test_native_decode_unaligned_mixed_fields_and_big_endian_rgba():
    fields = [NS(name=name, offset=offset, datatype=kind, count=1) for name, offset, kind in
              [("x", 1, 3), ("y", 3, 8), ("z", 11, 6), ("rgba", 15, 7)]]
    first = struct.pack(">xh d I I", -30, 1.125, 3_000_000_000, 0xAB0102FE)
    second = struct.pack(">xh d I I", 20, float("inf"), 17, 0)
    data = first + b"pad" + second + b"pad"
    msg = NS(width=1, height=2, point_step=19, row_step=22, fields=fields,
             data=np.frombuffer(data, dtype=np.uint8), is_bigendian=True)
    xyz, colors = decode(msg)
    np.testing.assert_array_equal(xyz, [[-30, 1.125, 3_000_000_000]])
    np.testing.assert_array_equal(colors, [[1, 2, 254]])
    assert xyz.flags.writeable and colors.flags.writeable
    assert memoryview(xyz.base.base).nbytes == 24


def test_native_range_colors_match_palette_interpolation():
    from planner_replay.cloud import scan_colors

    xyz = np.random.default_rng(73).normal(size=(4000, 3)) * 20
    position = [6., -3., 1.]
    limits = (2., 55.)
    cloud = CloudMap(range_color=True, range_limits=limits)
    cloud.xyz = xyz
    expected = scan_colors(np.linalg.norm(xyz-position, axis=1), limits)
    np.testing.assert_array_equal(cloud.colors_from(position), expected)
