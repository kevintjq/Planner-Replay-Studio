#!/usr/bin/env python3
"""Run with the ROS1 system Python; send subscribed messages to a local renderer."""
import argparse
import base64
import json
import queue
import socket
import threading
import rospy
from nav_msgs.msg import Odometry
from sensor_msgs.msg import PointCloud2
from tf2_msgs.msg import TFMessage


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--cloud-topic',default='/cloud_registered')
    p.add_argument('--odom-topic',default='/Odometry')
    p.add_argument('--port',type=int,default=8765)
    a=p.parse_args()
    from rospy_message_converter import message_converter
    rospy.init_node('slam_fpv_bridge',anonymous=True)
    pending=queue.Queue(maxsize=32)
    def callback(msg,topic):
        value=message_converter.convert_ros_message_to_dictionary(msg)
        # Converter versions differ on uint8[] representation; normalize it.
        if isinstance(msg,PointCloud2): value['data']=base64.b64encode(bytes(msg.data)).decode('ascii')
        try: pending.put_nowait(dict(topic=topic,message=value))
        except queue.Full: rospy.logwarn_throttle(2,'SLAM FPV bridge queue full: dropping message')
    sock=socket.create_connection(('127.0.0.1',a.port),timeout=10); sock.settimeout(None)
    def send():
        try:
            while not rospy.is_shutdown():
                try: value=pending.get(timeout=.5)
                except queue.Empty: continue
                sock.sendall((json.dumps(value,separators=(',',':'))+'\n').encode())
        except OSError as e: rospy.logerr(str(e)); rospy.signal_shutdown('renderer disconnected')
    threading.Thread(target=send,daemon=True).start()
    subscriptions=[rospy.Subscriber(topic,typ,callback,callback_args=topic,queue_size=8)
                   for topic,typ in [(a.cloud_topic,PointCloud2),(a.odom_topic,Odometry),('/tf',TFMessage),('/tf_static',TFMessage)]]
    try: rospy.spin()
    finally: sock.close()


if __name__=='__main__': main()
