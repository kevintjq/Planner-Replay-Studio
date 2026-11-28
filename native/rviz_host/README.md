# Native RViz backend

This is a C++ / Qt5 / ROS 1 Noetic / librviz Linux backend. The Windows Qt6 app
starts it through the user's configured WSL distribution. It is not a Windows
executable, a bundled ROS installation, or a ROS 2 implementation.

The packaged `uav-studio-rviz-host` is a Linux x86_64 ELF built against Ubuntu
20.04 / ROS Noetic, Qt5 and OGRE. A compatible Linux/WSL installation with ROS
Noetic, RViz, rosbag, topic_tools, tf2, visualization messages and working OpenGL
is required. The default ROS setup is `/opt/ros/noetic/setup.bash`; an explicitly
configured launch script may pass `--setup` for another compatible installation.
The app itself adds no Python runtime dependency; ROS keeps its own standard
runtime requirements.

`run_host.sh` prefers the ELF beside the script. In a source checkout without
that adjacent ELF it falls back to `build-native/rviz-host/uav-studio-rviz-host`.
All arguments are passed as separate process arguments. The wrapper owns an
isolated master, logs its session directory, refuses occupied ports, and only
cleans its own process groups. Do not point its simulated-time session at a
running robot's shared master.

To rebuild in a compatible Linux/WSL environment:

```sh
source /opt/ros/noetic/setup.bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
bash run_host.sh --host "$PWD/build/uav-studio-rviz-host" --hidden
```

Build sources are `CMakeLists.txt`, `main.cpp`, `bag_replay.h` and
`bag_replay.cpp`. The Python `verify_*.py` files are developer acceptance tools
and are not required to launch the application.

The JSON interface exposes actual installed plugins and native properties.
Header-time indexing and full-history paused seek have explicit time policies,
TF coverage and asynchronous-plugin limits. A framebuffer preview is not a
high-resolution export, and native continuous real-time playback is not yet
certified. See `docs/NATIVE_RVIZ_BACKEND_CN.md` in the source repository for API,
time semantics, evidence and remaining acceptance gates.
