import numpy as np
import pytest
from planner_replay.geometry import TransformTree,Series,matrix,camera,transform_points,slerp


def test_tf_chain_inverse_and_extrinsic():
    tree=TransformTree()
    tree.add('map','body',0,[2,3,4],[0,0,np.sqrt(.5),np.sqrt(.5)],True)
    tree.add('body','lidar',0,[1,0,0],[0,0,0,1],True)
    T=tree.lookup('map','lidar',42)
    np.testing.assert_allclose(transform_points(T,np.array([[1,0,0]])),[[2,5,4]],atol=1e-10)
    np.testing.assert_allclose(tree.lookup('lidar','map',42)@T,np.eye(4),atol=1e-10)


def test_interpolation_and_extrapolation():
    s=Series(); s.add(1_000_000_000,[0,0,0],[0,0,0,1]); s.add(2_000_000_000,[2,0,0],[0,0,1,0])
    T=s.at(1_500_000_000,1)
    np.testing.assert_allclose(T[:3,3],[1,0,0])
    np.testing.assert_allclose(T[:3,0],[0,1,0],atol=1e-10)
    with pytest.raises(LookupError): s.at(999,1)
    with pytest.raises(LookupError): s.at(1_500_000_000,.1)
    np.testing.assert_allclose(slerp([0,0,0,1],[0,0,0,-1],.5),[0,0,0,1])


def test_camera_exact_rear_and_oblique_yaw_only():
    T=matrix([1,2,3],[0,0,np.sqrt(.5),np.sqrt(.5)])
    p,target=camera(T)
    np.testing.assert_allclose(p, [1,2-3.8,4.35],atol=1e-10)
    np.testing.assert_allclose(target,[1,3.5,3],atol=1e-10)
    p,target=camera(T,10,6.7,0,0)
    elevation=np.rad2deg(np.arctan2(p[2]-target[2],np.linalg.norm(p[:2]-target[:2])))
    assert 33<elevation<35
    assert np.linalg.norm(p[:2]-target[:2])>9


def test_invalid_and_out_of_order():
    s=Series()
    with pytest.raises(ValueError): s.add(1,[0,0,0],[0,0,0,0])
    s.add(2,[2,0,0],[0,0,0,1]); s.add(1,[1,0,0],[0,0,0,1]); s.prepare()
    assert s.at(1,.5)[0,3]==1
    s.add(3,[3,0,0],[0,0,0,1]); assert s.at(3,.5)[0,3]==3
    with pytest.raises(LookupError): TransformTree().lookup('map','body',1)


def test_ambiguous_and_cyclic_tree_rejected():
    t=TransformTree()
    t.add('map','body',0,[0,0,0],[0,0,0,1],True)
    with pytest.raises(ValueError): t.add('odom','body',0,[0,0,0],[0,0,0,1],True)
    with pytest.raises(ValueError): t.add('body','map',0,[0,0,0],[0,0,0,1],True)
