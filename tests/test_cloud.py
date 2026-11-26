from types import SimpleNamespace as NS
import struct
import numpy as np
import pytest
from slam_fpv.cloud import decode,CloudMap


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
    from slam_fpv.cloud import CloudMap

    cloud = CloudMap(0, 5, scan_height=True, height_limits=(-1, 3))
    points = np.array([[0, 0, 0], [.001, 0, 0], [0, 0, 2]], dtype=float)
    cloud.add(points)
    first_colors = cloud.rgb.copy()
    assert len(cloud.xyz) == 3
    assert (first_colors[0] != first_colors[2]).any()
    cloud.add(points)
    assert len(cloud.xyz) == 5
    np.testing.assert_array_equal(cloud.rgb[-3:], first_colors)


def test_range_palette_uses_acquisition_origin_and_keeps_previous_colors():
    cloud = CloudMap(0, range_color=True, range_limits=(0., 10.))
    points = np.array([[8., 0., 0.], [4., 0., 4.]])
    cloud.add(points, origin=[4., 0., 0.])
    first_colors = cloud.rgb.copy()
    np.testing.assert_array_equal(first_colors[0], first_colors[1])
    cloud.add(points, origin=[8., 0., 0.])
    np.testing.assert_array_equal(cloud.rgb[:2], first_colors)
    assert not np.array_equal(cloud.rgb[2], cloud.rgb[3])
