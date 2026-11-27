"""Versioned desktop projects reference media files on disk."""
from dataclasses import asdict, dataclass, replace
import json
import os
import shutil
from pathlib import Path
from tempfile import NamedTemporaryFile, mkdtemp

from .composition import Composition, ImageLayer, LayerMotion, LayoutCue, PointCloudView, TextLayer, TimelineMarker, VideoLayer
from .footage import VideoEdit
from .topic_displays import TopicDisplay


WORKSPACE_PROJECT = "project.slamfpv"


@dataclass(frozen=True)
class ProjectFile:
    composition: Composition
    values: dict
    enabled: dict
    source: str
    bag: Path | None = None
    extrinsics: Path | None = None
    portable: bool = False

    def media(self):
        return tuple(dict.fromkeys(path for path in (self.bag, self.extrinsics,
                                   *(layer.path for layer in (*self.composition.videos, *self.composition.images))) if path))

    def relink(self, replacements):
        videos = tuple(replace(layer, path=replacements.get(layer.path, layer.path))
                       for layer in self.composition.videos)
        images = tuple(replace(layer, path=replacements.get(layer.path, layer.path))
                       for layer in self.composition.images)
        return replace(self, composition=replace(self.composition, videos=videos, images=images),
                       bag=replacements.get(self.bag, self.bag),
                       extrinsics=replacements.get(self.extrinsics, self.extrinsics))


def save_project(path, project):
    path = path.resolve()
    if path in {source.resolve() for source in project.media()}:
        raise ValueError("项目文件不能覆盖素材。")

    def reference(media):
        if not media:
            return None
        try:
            return Path(os.path.relpath(media.resolve(), path.parent)).as_posix()
        except ValueError:  # Windows paths on different drives
            return str(media.resolve())

    composition = asdict(project.composition)
    for layer in (*composition["videos"], *composition["images"]):
        layer["path"] = reference(layer["path"])
    data = {"version": 1, "source": project.source, "bag": reference(project.bag),
            "extrinsics": reference(project.extrinsics), "values": project.values,
            "enabled": project.enabled, "composition": composition}
    if project.portable:
        data["portable"] = True
    pending = None
    try:
        with NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as file:
            pending = Path(file.name)
            json.dump(data, file, ensure_ascii=False, indent=2, allow_nan=False)
            file.write("\n")
        pending.replace(path)
    finally:
        if pending is not None:
            pending.unlink(missing_ok=True)


def save_workspace_folder(folder, project, *, create, check_cancel=lambda: None, progress=lambda done, total: None):
    """Copy media before atomically publishing the project manifest."""
    folder = folder.parent.resolve() / folder.name
    manifest = folder / WORKSPACE_PROJECT
    if create:
        folder.mkdir()
    elif not load_project(manifest).portable:
        raise ValueError("此文件夹不是 Planner Replay 项目。")

    added = []
    try:
        for name in ("media", "cache", "exports"):
            directory = folder / name
            directory.mkdir(exist_ok=True)
            if directory.resolve() != directory:
                raise ValueError(f"项目目录不能指向外部位置：{name}")
        media = folder / "media"
        replacements, copied = {}, {}
        sources = project.media()
        for index, source in enumerate(sources, 1):
            check_cancel()
            original = source.resolve(strict=True)
            if not original.is_file():
                raise ValueError(f"素材不是文件：{source}")
            if original not in copied:
                if original.is_relative_to(media):
                    copied[original] = original
                else:
                    storage = Path(mkdtemp(prefix="asset-", dir=media))
                    added.append(storage)
                    target = storage / source.name
                    with original.open("rb") as incoming, target.open("xb") as outgoing:
                        while block := incoming.read(4 * 1024 * 1024):
                            check_cancel()
                            outgoing.write(block)
                    copied[original] = target
            replacements[source] = copied[original]
            progress(index, len(sources))
        check_cancel()
        saved = replace(project.relink(replacements), portable=True)
        save_project(manifest, saved)
        return manifest, saved, replacements
    except BaseException:
        if create:
            shutil.rmtree(folder, ignore_errors=True)
        else:
            for storage in added:
                shutil.rmtree(storage, ignore_errors=True)
        raise


def load_project(path):
    path = path.resolve()

    def media(value):
        return (path.parent / value).resolve() if value else None

    def vectors(data, *keys):
        for key in keys:
            if data.get(key) is not None:
                data[key] = tuple(data[key])
        return data

    def view(data):
        return PointCloudView(**vectors(data, "position", "overlays"))

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if data["version"] != 1:
            raise ValueError("不支持此项目文件版本。")
        source = data["source"]
        bag = media(data.get("bag"))
        if source not in {"bag", "demo", "none"} or (source == "bag") != (bag is not None):
            raise ValueError("项目的数据源无效。")
        values, enabled = data["values"], data["enabled"]
        if any(not isinstance(value, str) for value in values.values()) or any(
                type(value) is not bool for value in enabled.values()):
            raise ValueError("项目设置格式无效。")
        composition = vectors(data["composition"], "speed_position")
        composition["main_view"] = view(composition["main_view"])
        composition["views"] = tuple(view(item) for item in composition["views"])
        composition["texts"] = tuple(TextLayer(**vectors(item, "position")) for item in composition["texts"])
        videos = []
        for item in composition["videos"]:
            video_path = media(item["path"])
            if video_path is None:
                raise ValueError("项目缺少视频路径。")
            edit = vectors(item["edit"], "crop")
            edit["anchors"] = tuple(tuple(pair) for pair in edit["anchors"])
            if edit.get("segments") is not None:
                edit["segments"] = tuple(tuple(segment) for segment in edit["segments"])
            item.update(path=video_path, edit=VideoEdit(**edit))
            videos.append(VideoLayer(**vectors(item, "position", "backdrop_position")))
        composition["videos"] = tuple(videos)
        images = []
        for item in composition.get("images", ()):
            image_path = media(item["path"])
            if image_path is None:
                raise ValueError("项目缺少图片路径。")
            item["path"] = image_path
            images.append(ImageLayer(**vectors(item, "position")))
        composition["images"] = tuple(images)
        composition["layouts"] = tuple(LayoutCue(**vectors(item, "position")) for item in composition.get("layouts", ()))
        composition["motions"] = tuple(LayerMotion(**vectors(item, "large_position"))
                                       for item in composition.get("motions", ()))
        composition["markers"] = tuple(TimelineMarker(**item) for item in composition.get("markers", ()))
        composition["topic_displays"] = tuple(TopicDisplay(**item) for item in composition.get("topic_displays", ()))
        return ProjectFile(Composition(**composition), values, enabled, source, bag, media(data.get("extrinsics")), data.get("portable") is True)
    except (AttributeError, KeyError, TypeError) as error:
        raise ValueError("项目文件格式无效。") from error
