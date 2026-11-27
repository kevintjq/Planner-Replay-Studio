"""Loopback protocol smoke test; does not claim rospy/ROS-master validation."""
import base64
import json
import socket
import subprocess
import sys
import time
import numpy as np
import imageio.v2 as imageio


def test_loopback_bridge_protocol(tmp_path):
    with socket.socket() as s:
        s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
    output=tmp_path/'stream.mp4'
    process=subprocess.Popen([sys.executable,'-m','planner_replay.cli','listen','--output',str(output),
        '--port',str(port),'--delay','.1','--seconds','.5','--fps','8','--width','640','--height','360'],
        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    try:
        deadline=time.monotonic()+15
        while True:
            try: conn=socket.create_connection(('127.0.0.1',port),timeout=1); break
            except OSError:
                if time.monotonic()>deadline: raise TimeoutError('Listener did not start')
                if process.poll() is not None: raise RuntimeError(process.communicate())
                time.sleep(.05)
        def send(topic,msg): conn.sendall((json.dumps(dict(topic=topic,message=msg))+'\n').encode())
        def header(i): return dict(stamp=dict(secs=1,nsecs=i*100000000),frame_id='camera_init')
        with conn:
            for i in range(9):
                send('/Odometry',dict(header=header(i),child_frame_id='body',pose=dict(pose=dict(
                    position=dict(x=i*.1,y=0,z=1),orientation=dict(x=0,y=0,z=0,w=1)))))
            for i in range(8):
                cloud=dict(header=header(i),height=1,width=3,is_bigendian=False,point_step=12,row_step=36,
                    fields=[dict(name=n,offset=k*4,datatype=7,count=1) for k,n in enumerate(['x','y','z'])],
                    data=base64.b64encode(np.array([[0,0,0],[1,1,1],[2,0,0]],dtype='<f4').tobytes()).decode())
                send('/cloud_registered',cloud)
        stdout,stderr=process.communicate(timeout=20)
        assert process.returncode==0,(stdout,stderr)
        report=json.loads(output.with_suffix('.json').read_text())
        assert report['complete'] and report['frames']==4 and report['dropped_clouds']==0
        reader=imageio.get_reader(output)
        try: assert reader.count_frames()==4
        finally: reader.close()
    finally:
        if process.poll() is None: process.kill(); process.communicate()
