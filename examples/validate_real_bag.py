"""Cross-check matching world/body clouds; no ground-truth accuracy claim."""
import argparse
import json
import numpy as np
from planner_replay.sources import BagSource


def main():
    p=argparse.ArgumentParser()
    p.add_argument('bag')
    p.add_argument('--world-cloud',default='/cloud_registered')
    p.add_argument('--body-cloud',default='/cloud_registered_body')
    p.add_argument('--odom-topic',default='/Odometry')
    p.add_argument('--world',default='camera_init')
    p.add_argument('--body',default='body')
    a=p.parse_args()
    world=BagSource(a.bag,a.world_cloud,a.odom_topic,a.world,a.body).index()
    body=BagSource(a.bag,a.body_cloud,a.odom_topic,a.world,a.body,'tf').index()
    wi,bi=iter(world.clouds()),iter(body.clouds())
    count=0; max_xyz=0.; max_pose=0.
    try:
        w=next(wi,None); b=next(bi,None)
        while w is not None and b is not None:
            if w[0]<b[0]: w=next(wi,None); continue
            if b[0]<w[0]: b=next(bi,None); continue
            if w[1].shape!=b[1].shape: raise ValueError('Corresponding clouds have different shapes')
            if len(w[1]): max_xyz=max(max_xyz,float(np.max(np.abs(w[1]-b[1]))))
            max_pose=max(max_pose,float(np.max(np.abs(world.pose(w[0])-body.pose(w[0])))))
            count+=1; w=next(wi,None); b=next(bi,None)
    finally: wi.close(); bi.close()
    if not count: raise ValueError('No matching supported cloud timestamps')
    result=dict(matched_clouds=count,max_component_difference_m=max_xyz,
                max_pose_matrix_difference=max_pose,leading_body_cloud_skips=body.leading_cloud_skips,
                interpretation='Agreement between recorded paths, not absolute accuracy')
    print(json.dumps(result,indent=2))


if __name__=='__main__': main()
