import json
import subprocess
import sys
import imageio.v2 as imageio
import numpy as np
from slam_fpv.renderer import Renderer
from slam_fpv.cloud import CloudMap
from slam_fpv.sources import DemoSource


def test_edl_two_view_frame_and_rgb_axes():
    s=DemoSource(1); _,xyz,rgb=next(s.clouds())
    m=CloudMap(); m.add(xyz,rgb)
    r=Renderer(640,360)
    try:
        r.update_cloud(m)
        assert len(r.views)==2
        assert all(scene.GetPass().IsA('vtkEDLShading') for scene,_ in r.views)
        image=r.frame(s.pose(s.first),[[0,0,1],[.1,0,1]],'SYNTHETIC TEST')
        assert image.shape==(360,640,3) and image.std()>10
        inset=image[15:130,425:625]
        assert inset.std()>10
        for channel in range(3):
            other=np.delete(image,channel,axis=2).max(axis=2)
            assert ((image[:,:,channel]>170)&(image[:,:,channel].astype(int)-other>50)).sum()>8
    finally: r.close()


def test_video_export_frame_count_size_and_motion(tmp_path):
    output=tmp_path/'demo.mp4'
    subprocess.run([sys.executable,'-m','slam_fpv.cli','demo','--output',str(output),
                    '--seconds','1','--fps','8','--width','640','--height','360'],check=True,capture_output=True)
    r=imageio.get_reader(output)
    try:
        assert r.count_frames()==9
        assert r.get_meta_data()['fps']==8
        first,last=np.asarray(r.get_data(0)),np.asarray(r.get_data(8))
        assert first.shape==(360,640,3)
        assert np.abs(first.astype(float)-last).mean()>2
    finally: r.close()
    report=json.loads(output.with_suffix('.json').read_text())
    assert report['source']=='synthetic' and report['complete'] and report['frames']==9
