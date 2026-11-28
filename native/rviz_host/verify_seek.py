"""Developer-only real-recording seek acceptance; not an app runtime dependency."""
import argparse
import base64
import json
import hashlib
import math
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid

import rosbag
import rospy
from geometry_msgs.msg import TransformStamped
from tf2_msgs.msg import TFMessage
from visualization_msgs.msg import Marker, MarkerArray
from PIL import Image
import yaml


def nodes(root):
    yield root
    for child in root.get("children", []):
        yield from nodes(child)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--bag", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--port", default=49173, type=int)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "source": "actual user recording", "checks": [],
              "main_application_integration_verified": False,
              "realtime_playback_certified": False, "arbitrary_plugins_certified": False}
    messages = queue.Queue()
    token = str(uuid.uuid4())
    host = None
    sequence = 0

    def check(ok, name):
        report["checks"].append({"name": name, "passed": bool(ok)})
        if not ok:
            raise RuntimeError(name)

    try:
        with rosbag.Bag(args.bag) as bag:
            actual_topics = set(bag.get_type_and_topic_info().topics)
            cloud = [(message.header.stamp.to_nsec(), stamp.to_nsec())
                     for _, message, stamp in bag.read_messages(topics=["/cloud_registered"])]
        check(len(cloud) > 20, "Actual pointcloud Header index available")
        origin = cloud[0][0]
        report["reference_header_ns"] = str(origin)
        report["reference_record_ns"] = str(cloud[0][1])
        check(abs(cloud[0][1] - origin) > 1000000000, "Recording actually distinguishes Header and record clocks")
        with (output / "host.log").open("w") as log:
            host = subprocess.Popen(["bash", args.launcher, "--hidden", "--session-token", token,
                                     "--port", str(args.port), "--config", args.config],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                    text=True, bufsize=1)

            def read():
                for line in host.stdout:
                    try:
                        messages.put(json.loads(line))
                    except ValueError:
                        messages.put({"protocol_error": line})
                messages.put({"eof": host.poll()})

            reader = threading.Thread(target=read, daemon=True)
            reader.start()

            def next_message():
                message = messages.get(timeout=90)
                if "protocol_error" in message or "eof" in message or message.get("event") == "fatal":
                    raise RuntimeError(str(message))
                return message

            while next_message().get("event") != "ready":
                pass

            def request(command, **values):
                nonlocal sequence
                sequence += 1
                host.stdin.write(json.dumps(dict(command=command, request_id=sequence, **values)) + "\n")
                host.stdin.flush()
                while True:
                    result = next_message()
                    if result.get("request_id") == sequence:
                        return result

            def call(command, **values):
                result = request(command, **values)
                check(result.get("ok"), command + (": " + result.get("error", "") if not result.get("ok") else ""))
                return result["result"]

            rejected = request("bag.index", file=args.bag, time_basis="header", untimed_policy="reject")
            check(not rejected["ok"] and "Header" in rejected.get("error", ""), "Untimed actual rosout messages rejected without a guessed clock")
            native_tree = call("tree")
            explicit_topics = {"/cloud_registered", "/cloud_registered_body", "/Odometry", "/tf"}
            def enabled_topics(node, active=True, root=True):
                if not root and "display_enabled" in node:
                    active = active and node["display_enabled"]
                if not active:
                    return
                if node["native_type"].endswith("RosTopicProperty") and node.get("topic_name"):
                    explicit_topics.add(node["topic_name"])
                for child in node["children"]:
                    enabled_topics(child, active, False)
            enabled_topics(native_tree["displays"])
            report["explicit_index_topics"] = sorted(explicit_topics)
            original_config_hash = hashlib.sha256(Path(args.config).read_bytes()).hexdigest()
            report["original_config_sha256"] = original_config_hash
            index = call("bag.index", file=args.bag, time_basis="header", untimed_policy="reject",
                         topics=sorted(explicit_topics))
            report["index"] = index
            samples = []
            for seconds in [0.0, 8.0, 2.0, 8.0]:
                target = origin + int(seconds * 1e9)
                started = time.monotonic()
                result = call("bag.seek", target_ns=str(target))
                (output / ("seek-result-" + str(len(samples)) + ".json")).write_text(json.dumps(result, indent=2))
                check(result["clock_ns"] == str(target) == result["target_ns"], "Exact Header experiment target and ROS clock at " + str(seconds))
                check(result["history_from_beginning"] and result["native_queues_drained"], "History restored and queues drained at " + str(seconds))
                expected = sum(stamp <= target for stamp, _ in cloud)
                actual = next((item for item in result["topics"] if item["name"] == "/cloud_registered"), None)
                check(actual is not None and int(actual["published"]) == expected == int(actual["observer_received"]), "Exact pointcloud message delivery count at " + str(seconds))
                check(actual["last_header_ns"] == str(max(stamp for stamp, _ in cloud if stamp <= target)), "Latest pointcloud is not from the future at " + str(seconds))
                tree = call("tree")
                statuses = [node for node in nodes(tree["displays"]) if "status" in node]
                result["native_statuses"] = [{"name": n["name"], "status": n["status"], "value": n["display_value"]} for n in statuses]
                frame = call("frame", max_width=640)
                ready = result["tf_coverage_complete"] and result["required_tf_complete"] and result["native_scene_ready"]
                check(frame["clock_ns"] == str(target) and frame["experiment_playhead_synchronized"] == ready and
                      frame["synchronized_target_ns"] == (str(target) if ready else ""),
                      "Frame retains exact clock and refuses incomplete native TF/Display readiness")
                filename = "seek-" + str(len(samples)) + ".png"
                (output / filename).write_bytes(base64.b64decode(frame["data_base64"], validate=True))
                result["screenshot"] = filename
                result["wall_seconds"] = time.monotonic() - started
                samples.append(result)
            report["seeks"] = samples
            check(samples[1]["topics"] == samples[3]["topics"], "Backward seek then repeat restored identical per-topic history")
            check(Image.open(output / "seek-1.png").tobytes() == Image.open(output / "seek-3.png").tobytes(),
                  "Repeated actual Header target produced identical native framebuffer pixels")
            # Native message counters and TF statuses stay authentic and visible in report.
            cloud_status = [s for s in samples[-1]["native_statuses"] if s["name"] == "Topic" and "messages received" in s["value"].lower()]
            check(bool(cloud_status), "Actual native plugins report received data")
            check(not samples[-1]["native_scene_ready"] and not samples[-1]["required_tf_complete"],
                  "Original real configuration with unavailable Target Frame is explicitly rejected as not ready")
            # The user's config remains read-only. Make a named native-saved
            # acceptance copy, recording each deliberate correction separately.
            native_tree = call("tree")
            adjustments = []
            target_property = next(n for n in nodes(native_tree["views"])
                                   if n["name"] == "Target Frame" and n["native_type"].endswith("TfFrameProperty"))
            fixed_frame = native_tree["fixed_frame"]
            adjustments.append({"property": "Current View / Target Frame", "before": target_property["value"],
                                "after": fixed_frame, "reason": "Original target is absent from the recording's native TF buffer."})
            call("set", property_id=target_property["id"], value=fixed_frame)
            def active_displays(node, active=True, root=True):
                if not root and "display_enabled" in node:
                    active = active and node["display_enabled"]
                if not active:
                    return
                if node.get("plugin_class"):
                    yield node
                for child in node["children"]:
                    yield from active_displays(child, active, False)
            def own_properties(display):
                for child in display["children"]:
                    if "display_enabled" not in child:
                        yield child
                        yield from own_properties(child)
            for display in active_displays(native_tree["displays"]):
                    absent = [n.get("topic_name") for n in own_properties(display)
                              if n["native_type"].endswith("RosTopicProperty") and n.get("topic_name") and n["topic_name"] not in actual_topics]
                    if absent:
                        adjustments.append({"display": display["name"], "before": True, "after": False,
                                            "reason": "No recorded source for native topic(s): " + ", ".join(absent)})
                        call("set", property_id=display["id"], value=False)
            accepted_copy = output / "native-core-ready.rviz"
            call("save", file=str(accepted_copy))
            positive = call("bag.seek", target_ns=str(origin + 8 * 10**9))
            check(positive["native_scene_ready"] and positive["required_tf_complete"],
                  "Explicit corrected acceptance copy restores actual native frame and Display readiness")
            positive_frame = call("frame", max_width=640)
            check(positive_frame["experiment_playhead_synchronized"], "Corrected real configuration passes actual framebuffer readiness gate")
            (output / "actual-core-ready.png").write_bytes(base64.b64decode(positive_frame["data_base64"], validate=True))
            report["corrected_actual_configuration"] = {"file": accepted_copy.name, "adjustments": adjustments, "result": positive}
            check(hashlib.sha256(Path(args.config).read_bytes()).hexdigest() == original_config_hash,
                  "Original user configuration remained byte-for-byte unchanged")
            output_results = []
            for width, height in [(1920, 1080), (3840, 2160), (7680, 4320)]:
                rendered = call("render_frame", target_ns=str(origin + 8 * 10**9), width=width, height=height)
                check(rendered["native_render_texture"] and rendered["width"] == width and rendered["height"] == height
                      and rendered["clock_ns"] == str(origin + 8 * 10**9) and rendered["native_scene_ready"],
                      "Native RenderTexture renders exact actual target at " + str(width) + "x" + str(height))
                filename = "actual-native-output-" + str(width) + ".png"
                (output / filename).write_bytes(base64.b64decode(rendered.pop("data_base64"), validate=True))
                with Image.open(output / filename) as exported:
                    check(exported.size == (width, height) and len(set(exported.convert("RGB").resize((256, 144)).getdata())) > 100,
                          "Native high-resolution PNG contains actual nonempty scene pixels")
                rendered["file"] = filename
                output_results.append(rendered)
            report["native_render_texture_actual_output"] = output_results
            check(not request("render_frame", target_ns=str(origin + 2 * 10**9), width=1920, height=1080)["ok"],
                  "Native output rejects a requested timestamp different from the current exact target")
            check(not request("render_frame", target_ns=str(origin + 8 * 10**9), width=20000, height=20000)["ok"],
                  "Native output rejects dimensions beyond its explicit memory/GPU budget")
            advance_results = []
            for offset in [8033333333, 8066666667, 8100000000]:
                advanced = call("bag.advance", target_ns=str(origin + offset))
                check(advanced["native_scene_ready"] and advanced["replay_mode"] == "incremental-forward" and not advanced["native_scene_reset"],
                      "Native forward advance preserves scene and exact readiness at " + str(offset))
                advance_results.append(advanced)
            advanced_frame = call("render_frame", target_ns=str(origin + 8100000000), width=1280, height=720)
            advanced_png = base64.b64decode(advanced_frame["data_base64"], validate=True)
            (output / "actual-incremental-frame.png").write_bytes(advanced_png)
            full = call("bag.seek", target_ns=str(origin + 8100000000))
            full_frame = call("render_frame", target_ns=str(origin + 8100000000), width=1280, height=720)
            (output / "actual-incremental-full-reference.png").write_bytes(base64.b64decode(full_frame["data_base64"], validate=True))
            check(full["topics"] == advance_results[-1]["topics"], "Incremental native history delivery counts equal full reference restoration")
            check(Image.open(output / "actual-incremental-frame.png").tobytes() == Image.open(output / "actual-incremental-full-reference.png").tobytes(),
                  "Actual-recording incremental native output equals full-reference pixels at the same target")
            check(not request("bag.advance", target_ns=str(origin + 2 * 10**9))["ok"], "Incremental advance rejects backward targets")
            report["native_incremental_actual_recording"] = advance_results
            check(not request("bag.seek", target_ns="1")["ok"], "Out-of-index target rejected")
            # Controlled fixtures test native lifetime/DELETE and timeless static TF;
            # they are explicitly separate from the actual-recording evidence above.
            fixture_bag = output / "native-time-conformance.bag"
            fixture_config = output / "native-time-conformance.rviz"
            config = {"Visualization Manager": {"Class": "", "Enabled": True,
                "Global Options": {"Fixed Frame": "world", "Background Color": "24; 24; 24", "Frame Rate": 30},
                "Displays": [{"Class": "rviz/MarkerArray", "Name": "Native lifetime conformance", "Enabled": True,
                              "Marker Topic": "/timing_fixture_markers", "Queue Size": 100}],
                "Views": {"Current": {"Class": "rviz/TopDownOrtho", "Target Frame": "world", "Scale": 50,
                                       "Angle": 0, "X": 0, "Y": 0}}}}
            fixture_config.write_text(yaml.safe_dump(config))
            with rosbag.Bag(str(fixture_bag), "w") as fixture:
                transform = TransformStamped()
                transform.header.frame_id = "world"
                transform.child_frame_id = "drone"
                transform.transform.translation.x = 1
                transform.transform.rotation.w = 1
                fixture.write("/tf_static", TFMessage([transform]), rospy.Time(2000))
                for stamp, marker_id, action, lifetime, x, rgb in [
                        (100, 1, Marker.ADD, 2, -2, (1, 0, 0)),
                        (101, 2, Marker.ADD, 0, 2, (0, 1, 0)),
                        (103, 2, Marker.DELETE, 0, 2, (0, 1, 0)),
                        (105, 0, Marker.DELETEALL, 0, 0, (0, 0, 0))]:
                    marker = Marker()
                    marker.header.frame_id = "drone"
                    marker.header.stamp = rospy.Time(stamp)
                    marker.ns, marker.id = "explicit_conformance_fixture", marker_id
                    marker.type, marker.action = Marker.CUBE, action
                    marker.pose.position.x, marker.pose.orientation.w = x, 1
                    marker.scale.x = marker.scale.y = marker.scale.z = 1
                    marker.color.r, marker.color.g, marker.color.b = rgb
                    marker.color.a = 1
                    marker.lifetime = rospy.Duration(lifetime)
                    fixture.write("/timing_fixture_markers", MarkerArray([marker]), rospy.Time(1900 + stamp))
                transform.transform.translation.x = 50
                fixture.write("/tf_static", TFMessage([transform]), rospy.Time(2004))
            call("load", file=str(fixture_config))
            static_index = call("bag.index", file=str(fixture_bag), time_basis="header", untimed_policy="map_record",
                                anchor_record_ns=str(2000 * 10**9), anchor_header_ns=str(100 * 10**9))
            check(any(int(item["explicitly_mapped"]) == 2 for item in static_index["topics"] if item["name"] == "/tf_static"),
                  "Zero-stamp tf_static uses only the explicitly supplied record/Header anchor")
            fixture_results = []
            for seconds in [101.5, 102.5, 103.5, 101.5]:
                result = call("bag.seek", target_ns=str(int(seconds * 1e9)))
                frame = call("frame", max_width=640)
                filename = "conformance-" + str(len(fixture_results)) + ".png"
                (output / filename).write_bytes(base64.b64decode(frame["data_base64"], validate=True))
                pixels = list(Image.open(output / filename).convert("RGB").getdata())
                red = sum(r > 100 and r > g * 2 and r > b * 2 for r, g, b in pixels)
                green = sum(g > 100 and g > r * 2 and g > b * 2 for r, g, b in pixels)
                fixture_results.append({"target_seconds": seconds, "red_pixels": red, "green_pixels": green,
                                        "elapsed_ms": result["elapsed_ms"], "screenshot": filename})
            report["controlled_conformance_fixture"] = fixture_results
            check(fixture_results[0]["red_pixels"] > 10 and fixture_results[0]["green_pixels"] > 10,
                  "Static TF prewarm renders both real native markers before lifetime expiry")
            check(fixture_results[1]["red_pixels"] == 0 and fixture_results[1]["green_pixels"] > 10,
                  "Native Marker lifetime expires at the selected experiment clock")
            check(fixture_results[2]["red_pixels"] == fixture_results[2]["green_pixels"] == 0,
                  "Native Marker DELETE and lifetime both apply at later selected time")
            check(fixture_results[3]["red_pixels"] == fixture_results[0]["red_pixels"] and fixture_results[3]["green_pixels"] == fixture_results[0]["green_pixels"],
                  "Backward replay restores expired/deleted markers without future static TF leakage")
            marker_advance = []
            for seconds in [102.5, 103.5]:
                advanced = call("bag.advance", target_ns=str(int(seconds * 1e9)))
                frame = call("render_frame", target_ns=str(int(seconds * 1e9)), width=640, height=360)
                filename = "incremental-marker-" + str(seconds) + ".png"
                (output / filename).write_bytes(base64.b64decode(frame["data_base64"], validate=True))
                pixels = list(Image.open(output / filename).convert("RGB").getdata())
                red = sum(r > 100 and r > g * 2 and r > b * 2 for r, g, b in pixels)
                green = sum(g > 100 and g > r * 2 and g > b * 2 for r, g, b in pixels)
                check(advanced["native_scene_ready"] and red == 0 and (green > 10 if seconds == 102.5 else green == 0),
                      "Incremental native scene applies Marker lifetime/DELETE at " + str(seconds))
                marker_advance.append(advanced)
            report["native_incremental_marker_control"] = marker_advance
            call("bag.seek", target_ns=str(104500000000))
            replaced_static = call("transform", frame="drone", target_frame="world", time_ns=str(104500000000))
            check(abs(replaced_static["translation"][0] - 50) < 1e-9, "Later static replacement applies at its explicit mapped time")
            call("bag.seek", target_ns=str(101500000000))
            prior_static = call("transform", frame="drone", target_frame="world", time_ns=str(101500000000))
            check(abs(prior_static["translation"][0] - 1) < 1e-9, "Backward seek removes a later static replacement")

            # A separate native fixture tests arbitrary targets between real TF
            # samples, including quaternion interpolation and camera following.
            dynamic_bag = output / "native-tf-bracketing.bag"
            dynamic_config = output / "native-tf-bracketing.rviz"
            config["Visualization Manager"]["Views"]["Current"]["Target Frame"] = "drone"
            dynamic_config.write_text(yaml.safe_dump(config))
            with rosbag.Bag(str(dynamic_bag), "w") as fixture:
                fixed = TransformStamped()
                fixed.header.frame_id, fixed.child_frame_id = "world", "base"
                fixed.transform.translation.x, fixed.transform.rotation.w = 1, 1
                fixture.write("/tf_static", TFMessage([fixed]), rospy.Time(2000))
                for stamp, x, yaw in [(100, 0, 0), (102, 4, math.pi / 2), (104, 8, math.pi)]:
                    moving = TransformStamped()
                    moving.header.frame_id, moving.child_frame_id = "base", "drone"
                    moving.header.stamp = rospy.Time(stamp)
                    moving.transform.translation.x = x
                    moving.transform.rotation.z, moving.transform.rotation.w = math.sin(yaw / 2), math.cos(yaw / 2)
                    fixture.write("/tf", TFMessage([moving]), rospy.Time(stamp + 1900))
                marker = Marker()
                marker.header.frame_id, marker.header.stamp = "drone", rospy.Time(100)
                marker.ns, marker.id = "dynamic_tf_conformance", 1
                marker.type, marker.action, marker.frame_locked = Marker.CUBE, Marker.ADD, True
                marker.pose.orientation.w = 1
                marker.scale.x = marker.scale.y = marker.scale.z = .8
                marker.color.g = marker.color.a = 1
                fixture.write("/timing_fixture_markers", MarkerArray([marker]), rospy.Time(2000))
                marker.header.stamp, marker.action = rospy.Time(105), Marker.DELETEALL
                fixture.write("/timing_fixture_markers", MarkerArray([marker]), rospy.Time(2005))
                marker.header.stamp, marker.action = rospy.Time(100), Marker.ADD
                marker.header.frame_id = "unrecorded_sensor"
                fixture.write("/missing_transform_markers", MarkerArray([marker]), rospy.Time(2000))
            call("load", file=str(dynamic_config))
            conflict = request("bag.index", file=str(dynamic_bag), time_basis="header", untimed_policy="map_record",
                               anchor_record_ns=str(2000 * 10**9), anchor_header_ns=str(100 * 10**9))
            check(not conflict["ok"] and "static/dynamic" in conflict.get("error", ""),
                  "Noetic static/dynamic cache-type conflict across sources is rejected explicitly")
            call("shutdown")
            host.wait(timeout=15)
            reader.join(timeout=2)
            messages = queue.Queue()
            token = str(uuid.uuid4())
            host = subprocess.Popen(["bash", args.launcher, "--hidden", "--session-token", token,
                                     "--port", str(args.port), "--config", str(dynamic_config)],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                    text=True, bufsize=1)
            reader = threading.Thread(target=read, daemon=True)
            reader.start()
            while next_message().get("event") != "ready":
                pass
            call("bag.index", file=str(dynamic_bag), time_basis="header", untimed_policy="map_record",
                 anchor_record_ns=str(2000 * 10**9), anchor_header_ns=str(100 * 10**9))
            dynamic_results = []
            for seconds, expected_x, expected_yaw in [(101, 3, math.pi / 4), (103, 7, 3 * math.pi / 4), (101, 3, math.pi / 4)]:
                target = str(seconds * 10**9)
                result = call("bag.seek", target_ns=target)
                report["last_dynamic_readiness_debug"] = result
                transform = call("transform", frame="drone", target_frame="world", time_ns=target)
                check(result["tf_coverage_complete"] and not result["future_raw_tf_in_render_buffer"],
                      "Dynamic target has complete native TF coverage without a future latest pose")
                check(result["required_tf_complete"] and result["native_scene_ready"],
                      "Native configured frame chains and Display status are ready at the arbitrary target")
                check(abs(transform["translation"][0] - expected_x) < 1e-9 and
                      abs(transform["rotation_xyzw"][2] - math.sin(expected_yaw / 2)) < 1e-9,
                      "Native tf2 translation/quaternion interpolate the arbitrary target")
                check(transform["frame_manager_zero_time_available"] and transform["frame_manager_time_ns"] == target and
                      abs(transform["frame_manager_zero_time_translation"][0] - expected_x) < 1e-5,
                      "Native FrameManager zero-time queries use the exact experiment target")
                check(abs(transform["camera_world_position"][0] - expected_x) < 1e-5,
                      "Native Target Frame camera follows the interpolated experiment position")
                frame = call("frame", max_width=640)
                filename = "tf-bracketing-" + str(len(dynamic_results)) + ".png"
                (output / filename).write_bytes(base64.b64decode(frame["data_base64"], validate=True))
                transform["screenshot"] = filename
                transform["samples"] = result["tf_target_samples"]
                dynamic_results.append(transform)
            report["dynamic_tf_conformance_fixture"] = dynamic_results
            check(Image.open(output / "tf-bracketing-0.png").tobytes() == Image.open(output / "tf-bracketing-2.png").tobytes(),
                  "Backward arbitrary-target TF/camera replay restores identical framebuffer pixels")
            readiness_cases = []
            def frame_property(root, name):
                return next(n for n in nodes(root) if n["name"] == name and n["native_type"].endswith("TfFrameProperty"))

            native_tree = call("tree")
            fixed_property = frame_property(native_tree["displays"], "Fixed Frame")
            target_property = frame_property(native_tree["views"], "Target Frame")
            for property_, missing_name in [(fixed_property, "unrecorded_fixed"), (target_property, "unrecorded_target")]:
                previous = property_["value"]
                call("set", property_id=property_["id"], value=missing_name)
                incomplete = call("bag.seek", target_ns="103000000000")
                frame = call("frame", max_width=640)
                check(incomplete["tf_coverage_complete"] and not incomplete["required_tf_complete"]
                      and not incomplete["native_scene_ready"] and not frame["experiment_playhead_synchronized"],
                      "Missing native " + property_["name"] + " cannot pass readiness despite valid recorded TF samples")
                readiness_cases.append({"kind": property_["name"], "result": incomplete})
                call("set", property_id=property_["id"], value=previous)

            display = call("add", **{"class": "rviz/MarkerArray", "name": "Missing required Display transform", "enabled": True})["display"]
            topic_property = next(n for n in nodes(display) if n["native_type"].endswith("RosTopicProperty"))
            call("set", property_id=topic_property["id"], value="/missing_transform_markers")
            incomplete = call("bag.seek", target_ns="103000000000")
            frame = call("frame", max_width=640)
            check(incomplete["tf_coverage_complete"] and not incomplete["native_scene_ready"]
                  and bool(incomplete["native_readiness"]["display_issues"]) and not frame["experiment_playhead_synchronized"],
                  "Enabled Display with entirely missing message TF chain cannot claim readiness")
            readiness_cases.append({"kind": "missing Display message TF", "result": incomplete})
            call("set", property_id=topic_property["id"], value="/never_recorded_topic")
            incomplete = call("bag.seek", target_ns="103000000000")
            check(not incomplete["native_scene_ready"] and any(issue.get("source") == "indexed-topic-delivery"
                  for issue in incomplete["native_readiness"]["display_issues"]),
                  "Enabled native Topic Property without delivered indexed data cannot claim readiness")
            readiness_cases.append({"kind": "missing native topic", "result": incomplete})
            call("remove", property_id=display["id"])
            restored = call("bag.seek", target_ns="103000000000")
            check(restored["native_scene_ready"], "Removing incompatible optional Display restores authentic native readiness")
            report["required_native_readiness_fixture"] = readiness_cases
            beyond = call("bag.seek", target_ns=str(104500000000))
            check(not beyond["tf_coverage_complete"], "Missing right TF bracket is reported explicitly")
            missing = request("transform", frame="drone", target_frame="world", time_ns=str(104500000000))
            report["missing_bracket_debug"] = {"seek": beyond, "transform": missing}
            check(not missing["ok"],
                  "Missing TF bracket is not silently extrapolated or held as an exact target transform")
            call("shutdown")
            host.wait(timeout=15)
        report["passed"] = True
    finally:
        if host and host.poll() is None:
            subprocess.run(["bash", args.launcher, "--stop-session", token], timeout=5, check=False)
            try:
                host.wait(timeout=15)
            except subprocess.TimeoutExpired:
                host.terminate()
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"]),
                      "index_ms": report["index"]["elapsed_ms"],
                      "seek_ms": [item["elapsed_ms"] for item in report["seeks"]]}))


if __name__ == "__main__":
    main()
