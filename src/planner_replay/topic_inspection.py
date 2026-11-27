"""Inspect the latest recorded message of any topic, including custom ROS types."""
from dataclasses import dataclass, fields, is_dataclass
from math import isfinite
from io import BytesIO

import numpy as np
from rosbags.highlevel import AnyReader
from PIL import Image


@dataclass(frozen=True)
class TopicMessage:
    kind: str
    time: float
    value: object
    raw: bool = False


def read_topic_at(path, topic, seconds, main_topic, check_cancel):
    """Use recorded time relative to the first recorded message of the main cloud."""
    if not isfinite(seconds) or seconds < 0:
        raise ValueError("查看时间须为非负数。")
    with AnyReader([path]) as reader:
        connections = [item for item in reader.connections if item.topic == topic]
        clock = [item for item in reader.connections if item.topic == main_topic]
        if not connections:
            raise ValueError(f"录包中找不到话题 {topic}。")
        origin = reader.start_time
        if clock:
            with_messages = reader.messages(connections=clock)
            try:
                first = next(with_messages, None)
                if first is not None:
                    origin = first[1]
            finally:
                with_messages.close()

        stop = min(reader.end_time, origin + round(seconds * 1e9) + 1)
        span = 500_000_000
        while stop > reader.start_time:
            start = max(reader.start_time, stop - span)
            latest = None
            for entry in reader.messages(connections=connections, start=start, stop=stop):
                check_cancel()
                latest = entry
            if latest is not None:
                connection, recorded, payload = latest
                relative = (recorded - origin) / 1e9
                try:
                    return TopicMessage(connection.msgtype, relative, reader.deserialize(payload, connection.msgtype))
                except Exception:
                    return TopicMessage(connection.msgtype, relative, payload, raw=True)
            stop = start
            span *= 2
            check_cancel()
    return None


PAGE_SIZE = 128


@dataclass(frozen=True)
class ArrayPage:
    array: object
    first: int
    stop: int


def message_children(value):
    if isinstance(value, ArrayPage):
        values, first, stop = value.array, value.first, value.stop
    elif isinstance(value, (np.ndarray, list, tuple, bytes, memoryview)):
        values, first, stop = value, 0, len(value)
    elif is_dataclass(value):
        yield from ((item.name, getattr(value, item.name)) for item in fields(value) if item.name != "__msgtype__")
        return
    else:
        return
    if stop - first > PAGE_SIZE:
        step = max(PAGE_SIZE, (stop - first + PAGE_SIZE - 1) // PAGE_SIZE)
        for i in range(first, stop, step):
            end = min(stop, i + step)
            yield f"[{i}–{end - 1}]", ArrayPage(values, i, end)
    else:
        for i in range(first, stop):
            yield f"[{i}]", values[i]


def message_summary(value):
    if isinstance(value, ArrayPage):
        return f"{value.stop - value.first:,} 项"
    if isinstance(value, np.ndarray):
        return f"{value.dtype} · {' × '.join(map(str, value.shape))}"
    if isinstance(value, (list, tuple, bytes, memoryview)):
        return f"{len(value):,} 项"
    if is_dataclass(value):
        return getattr(value, "__msgtype__", type(value).__name__)
    return str(value)


def image_preview(message, size=(620, 245)):
    """Decode standard ROS camera pixels while retaining the original message fields."""
    if message.raw:
        return None
    kind, value = message.kind, message.value
    if kind == "sensor_msgs/msg/CompressedImage":
        try:
            with Image.open(BytesIO(value.data)) as source:
                source.draft("RGB", size)
                image = source.convert("RGB")
        except (OSError, ValueError, Image.DecompressionBombError):
            return None
    elif kind == "sensor_msgs/msg/Image":
        formats = {"mono8": ("L", "L", 1), "8uc1": ("L", "L", 1),
                   "rgb8": ("RGB", "RGB", 3), "bgr8": ("RGB", "BGR", 3),
                   "rgba8": ("RGBA", "RGBA", 4), "bgra8": ("RGBA", "BGRA", 4)}
        pixel_format = formats.get(value.encoding.lower())
        if pixel_format is None or value.width <= 0 or value.height <= 0:
            return None
        mode, raw, channels = pixel_format
        if value.step < value.width * channels or len(value.data) < value.height * value.step:
            return None
        image = Image.frombytes(mode, (value.width, value.height), value.data.tobytes(),
                                "raw", raw, value.step, 1)
    else:
        return None
    image.thumbnail(size, Image.Resampling.LANCZOS)
    return image
