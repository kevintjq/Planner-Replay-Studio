"""Windows bundle entry point. Normal startup uses the composition workspace."""

from pathlib import Path
from tempfile import TemporaryDirectory
import sys
import traceback

from planner_replay.gui import main


def smoke():
    import tkinter as tk

    import av
    import cv2
    import imageio.v2 as imageio
    import numpy as np
    import vtk
    from PIL import Image
    from rosbags.highlevel import AnyReader
    from rosbags.rosbag1 import Writer
    from rosbags.typesys import Stores, get_typestore
    from planner_replay._ghost_mask import rgb_difference
    from planner_replay.cloud import first_voxel_indices
    from planner_replay.composition import Composition, ImageLayer, TextLayer, VideoLayer, compose
    from planner_replay.gui import RendererApp
    from planner_replay.project_file import ProjectFile, load_project, save_project, save_workspace_folder
    from planner_replay.rviz_config import read_rviz
    from planner_replay.topic_inspection import read_topic_at

    assert vtk.vtkVersion.GetVTKMajorVersion() >= 9
    assert cv2.TrackerCSRT_create() is not None
    assert rgb_difference(bytes([10, 40, 30]), bytes([9, 20, 31])) == bytes([20])
    assert first_voxel_indices([[.1, 0., 0.], [.2, 0., 0.], [-.1, 0., 0.]], 1.).tolist() == [2, 0]
    root = tk.Tk()
    try:
        root.withdraw()
        app = RendererApp(root)
        root.update_idletasks()
        assert app.timeline is not None and app.spatial_topics["plan"] is not None
        app.edit_speed()
        assert app.nav.select() == str(app.settings_page)
        app.close_settings()
    finally:
        root.destroy()

    with TemporaryDirectory() as folder:
        folder = Path(folder)
        bag = folder / "smoke.bag"
        schema = get_typestore(Stores.ROS1_NOETIC)
        kind = "std_msgs/msg/String"
        with Writer(bag) as writer:
            topic = writer.add_connection("/smoke", kind, typestore=schema)
            writer.write(topic, 10**9, schema.serialize_ros1(schema.types[kind]("replay"), kind))
        with AnyReader([bag]) as reader:
            topic, _, raw = next(reader.messages())
            assert reader.deserialize(raw, topic.msgtype).data == "replay"

        config = folder / "smoke.rviz"
        config.write_text("Visualization Manager:\n  Global Options: {Fixed Frame: map}\n"
                          "  Displays: [{Class: example/Custom, Topic: /smoke}]\n", encoding="utf-8")
        rviz = read_rviz(config)
        sample = read_topic_at(bag, "/smoke", 0, "", lambda: None)
        assert rviz.fixed_frame == "map" and rviz.displays[0].kind == "message" and sample.value.data == "replay"

        project = Composition(show_speed=True, texts=(TextLayer("Smoke", 0., 1.),),
                              topic_displays=rviz.displays, display_fixed_frame=rviz.fixed_frame)
        project_path = folder / "smoke.slamfpv"
        save_project(project_path, ProjectFile(project, {}, {}, "demo"))
        restored = load_project(project_path).composition
        frame, layers = compose(Image.new("RGB", (320, 180)), restored, (), .25, (3., 4., 2.))
        assert restored == project and ("speed", 0) in layers and ("text", 0) in layers
        path = folder / "smoke.mp4"
        with imageio.get_writer(path, fps=60, codec="libx264", macro_block_size=2) as writer:
            writer.append_data(np.asarray(frame.convert("RGB")))
        with av.open(str(path)) as video:
            assert video.streams.video[0].average_rate == 60 and next(video.decode(video=0)) is not None

        asset = folder / "标注.png"
        frame.save(asset)
        composition = Composition(videos=(VideoLayer(path),), images=(ImageLayer(asset, 0., 1.),),
                                  texts=restored.texts, show_speed=True)
        project = ProjectFile(composition, {}, {}, "bag", bag)
        manifest, _, _ = save_workspace_folder(folder / "项目", project, create=True)
        moved = manifest.parent.with_name("移动后的项目")
        manifest.parent.rename(moved)
        for original in project.media():
            original.unlink()
        reopened = load_project(moved / manifest.name)
        assert reopened.portable and (moved / "cache").is_dir() and (moved / "exports").is_dir()
        for item in reopened.media():
            assert item.is_file(), f"Missing relocated media: {item}"
            assert item.is_relative_to(moved / "media"), f"Media escaped {moved / 'media'}: {item}"
        with av.open(str(reopened.composition.videos[0].path)) as video:
            assert video.streams.video[0].average_rate == 60
        with AnyReader([reopened.bag]) as reader:
            topic, _, raw = next(reader.messages())
            assert reader.deserialize(raw, topic.msgtype).data == "replay"



if __name__ == "__main__":
    if sys.argv[1:2] == ["--smoke"]:
        log = Path(sys.argv[2]) if len(sys.argv) > 2 else None
        try:
            smoke()
        except Exception:
            if log is not None:
                log.write_text(traceback.format_exc(), encoding="utf-8")
            raise SystemExit(1)
        if log is not None:
            log.write_text("Workspace, ROS1 bag, project round-trip and relocation, overlays and 60 fps encode/decode passed.\n",
                           encoding="utf-8")
    else:
        main()
