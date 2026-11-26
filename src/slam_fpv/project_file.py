"""Versioned desktop projects reference media files on disk."""
from dataclasses import asdict, dataclass, replace
import json
import os
from pathlib import Path
from tempfile import NamedTemporaryFile

from .composition import Composition, PointCloudView, TextLayer, VideoLayer
from .footage import VideoEdit


@dataclass(frozen=True)
class ProjectFile:
    composition: Composition
    values: dict
    enabled: dict
    source: str
    bag: Path | None = None
    extrinsics: Path | None = None

    def media(self):
        return tuple(dict.fromkeys(path for path in (self.bag, self.extrinsics,
                                   *(layer.path for layer in self.composition.videos)) if path))

    def relink(self, replacements):
        videos = tuple(replace(layer, path=replacements.get(layer.path, layer.path))
                       for layer in self.composition.videos)
        return replace(self, composition=replace(self.composition, videos=videos),
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
            return os.path.relpath(media.resolve(), path.parent)
        except ValueError:  # Windows paths on different drives
            return str(media.resolve())

    composition = asdict(project.composition)
    for layer in composition["videos"]:
        layer["path"] = reference(layer["path"])
    data = {"version": 1, "source": project.source, "bag": reference(project.bag),
            "extrinsics": reference(project.extrinsics), "values": project.values,
            "enabled": project.enabled, "composition": composition}
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
            item.update(path=video_path, edit=VideoEdit(**edit))
            videos.append(VideoLayer(**vectors(item, "position", "backdrop_position")))
        composition["videos"] = tuple(videos)
        return ProjectFile(Composition(**composition), values, enabled, source, bag, media(data.get("extrinsics")))
    except (AttributeError, KeyError, TypeError) as error:
        raise ValueError("项目文件格式无效。") from error
