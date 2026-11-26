from dataclasses import replace
import json

from slam_fpv.composition import Composition, LayoutCue, PointCloudView, VideoLayer
from slam_fpv.footage import VideoEdit
from slam_fpv.project_file import ProjectFile, load_project, save_project


def test_round_trip_keeps_per_clip_anchors_and_per_view_overlays(tmp_path):
    folder = tmp_path / "资料"
    folder.mkdir()
    path = folder / "演示.slamfpv"
    bag, extrinsics = folder / "flight.bag", folder / "tf.json"
    first, second = folder / "left.mp4", folder / "right.mp4"
    composition = Composition(
        main_view=PointCloudView("rear", 1., (.5, .5), overlays=("actual", "scan"), zoom=1.6),
        views=(PointCloudView("oblique", overlays=("plan", "corridor")), PointCloudView("fpv", overlays=())),
        videos=(VideoLayer(first, VideoEdit(trim_in=1., anchors=((0., 2.), (6., 8.2)))),
                VideoLayer(second, VideoEdit(crop=(.1, .1, .9, .9), anchors=((0., 1.), (6., 7.))))),
        transparent_canvas=True,
    )
    clip = composition.videos[1]
    composition = replace(composition, layouts=(LayoutCue(1., "video", clip.id), LayoutCue(2.5, "mix")),
                          audio_id=clip.id, play_rate=.5, compare_plans=True)
    saved = ProjectFile(composition, {"encoder": "CPU", "cloud_topic": "/registered"},
                        {"scan": True, "camera": False}, "bag", bag, extrinsics)
    save_project(path, saved)
    assert json.loads(path.read_text())["composition"]["videos"][0]["path"] == first.name
    restored = load_project(path)
    assert restored == saved
    replacement = folder / "moved.mp4"
    relinked = restored.relink({second: replacement})
    assert relinked.composition.videos[0] == saved.composition.videos[0]
    assert relinked.composition.videos[1] == replace(saved.composition.videos[1], path=replacement)
