from dataclasses import replace
from pathlib import Path
import json

import pytest

from slam_fpv.composition import Composition, ImageLayer, LayerMotion, LayoutCue, PointCloudView, TimelineMarker, VideoLayer
from slam_fpv.footage import VideoEdit
from slam_fpv.project_file import ProjectFile, load_project, save_project, save_workspace_folder


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
                VideoLayer(second, VideoEdit(crop=(.1, .1, .9, .9), anchors=((0., 1.), (6., 7.)),
                                            segments=((1., 2.), (3., None))))),
        transparent_canvas=True,
    )
    clip = composition.videos[1]
    composition = replace(composition, layouts=(LayoutCue(1., "backdrop", clip.id),
                                               LayoutCue(2.5, "inset", clip.id, .65, .28, (.15, .2)), LayoutCue(5., "mix")),
                          motions=(LayerMotion("view", composition.views[0].id, 0., 1.5, .8, .9, (.2, .7)),),
                          audio_id=clip.id, play_rate=.5, compare_plans=True, markers=(TimelineMarker(1.2, '入口'),))
    saved = ProjectFile(composition, {"encoder": "CPU", "cloud_topic": "/registered"},
                        {"scan": True, "camera": False}, "bag", bag, extrinsics)
    save_project(path, saved)
    assert json.loads(path.read_text(encoding="utf-8"))["composition"]["videos"][0]["path"] == first.name
    restored = load_project(path)
    assert restored == saved
    replacement = folder / "moved.mp4"
    relinked = restored.relink({second: replacement})
    assert relinked.composition.videos[0] == saved.composition.videos[0]
    assert relinked.composition.videos[1] == replace(saved.composition.videos[1], path=replacement)


def test_image_layers_save_relative_paths_and_relink(tmp_path):
    asset = tmp_path / "assets" / "徽标.png"
    project = ProjectFile(Composition(images=(ImageLayer(asset, 1., 2.),)), {}, {}, "none")
    file = tmp_path / "saved.slamfpv"
    save_project(file, project)
    assert Path(json.loads(file.read_text(encoding="utf-8"))["composition"]["images"][0]["path"]) == Path("assets") / "徽标.png"
    restored = load_project(file)
    assert restored == project and restored.media() == (asset,)
    moved = tmp_path / "new.png"
    assert restored.relink({asset: moved}).media() == (moved,)


def test_folder_survives_move_without_originals_and_saves_new_assets(tmp_path):
    originals = tmp_path / "原始素材"
    originals.mkdir()
    left, right = (originals / name / "flight.mp4" for name in ("left", "right"))
    for video in (left, right):
        video.parent.mkdir()
        video.write_bytes(video.parent.name.encode())
    bag, tf, image = (originals / name for name in ("input.bag", "tf.json", "图.png"))
    for path in (bag, tf, image):
        path.write_bytes(path.name.encode())
    composition = Composition(videos=(VideoLayer(left, VideoEdit(anchors=((0., 1.),))),
                                      VideoLayer(right), VideoLayer(left)),
                              images=(ImageLayer(image, 1., 2.),), markers=(TimelineMarker(1.2, "入口"),))
    project = ProjectFile(composition, {"cloud_topic": "/map"}, {"scan": True}, "bag", bag, tf)
    folder = tmp_path / "论文演示"
    manifest, saved, mapping = save_workspace_folder(folder, project, create=True)
    assert len(mapping) == 5 and saved.portable
    assert saved.composition.videos[0].path == saved.composition.videos[2].path
    assert saved.composition.videos[0].path != saved.composition.videos[1].path
    assert all(path.is_relative_to(folder / "media") for path in saved.media())
    assert (folder / "cache").is_dir() and (folder / "exports").is_dir()

    relocated = tmp_path / "随项目搬走"
    folder.rename(relocated)
    originals.rename(tmp_path / "unavailable")
    reopened = load_project(relocated / manifest.name)
    assert [path.read_bytes() for path in reopened.media()] == [
        b"input.bag", b"tf.json", b"left", b"right", "图.png".encode(),
    ]
    assert reopened.composition.markers == composition.markers
    assert reopened.composition.videos[0].edit == composition.videos[0].edit

    fresh = tmp_path / "new.png"
    fresh.write_bytes(b"new")
    edited = replace(reopened, composition=replace(reopened.composition, images=(ImageLayer(fresh, 0., 1.),)))
    _, updated, _ = save_workspace_folder(relocated, edited, create=False)
    assert updated.composition.images[0].path.read_bytes() == b"new"
    assert updated.composition.videos == reopened.composition.videos
    assert len(list((relocated / "media").iterdir())) == 6


def test_cancelled_copy_preserves_existing_project_and_new_location_is_clean(tmp_path):
    image = tmp_path / "image.png"
    image.write_bytes(b"image")
    new = tmp_path / "new.png"
    new.write_bytes(b"new image")
    project = ProjectFile(Composition(images=(ImageLayer(image, 0., 1.),)), {}, {}, "none")
    folder = tmp_path / "original"
    manifest, saved, _ = save_workspace_folder(folder, project, create=True)
    edited = replace(saved, composition=replace(saved.composition, images=(ImageLayer(new, 0., 1.),)))

    def cancel_after_copy(_done, _total):
        raise InterruptedError("user cancelled")

    with pytest.raises(InterruptedError):
        save_workspace_folder(folder, edited, create=False, progress=cancel_after_copy)
    assert load_project(manifest) == saved
    assert list((folder / "media").iterdir()) == [saved.composition.images[0].path.parent]
    cancelled = tmp_path / "cancelled"
    with pytest.raises(InterruptedError):
        save_workspace_folder(cancelled, project, create=True, progress=cancel_after_copy)
    assert not cancelled.exists()
    with pytest.raises(FileExistsError):
        save_workspace_folder(folder, project, create=True)
    assert load_project(manifest) == saved


def test_staged_output_can_publish_across_drives(tmp_path, monkeypatch):
    from errno import EXDEV

    from slam_fpv.export_media import publish_output

    cache = tmp_path / "cache"
    cache.mkdir()
    staged = cache / "video.mp4"
    staged.write_bytes(b"new")
    destination = tmp_path / "video.mp4"
    destination.write_bytes(b"old")
    replace_file = Path.replace

    def cross_drive(source, target):
        if source == staged:
            raise OSError(EXDEV, "different drive")
        return replace_file(source, target)

    monkeypatch.setattr(Path, "replace", cross_drive)
    publish_output(staged, destination)
    assert destination.read_bytes() == b"new"
    assert set(tmp_path.iterdir()) == {cache, destination}
