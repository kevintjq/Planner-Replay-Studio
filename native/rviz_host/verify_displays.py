"""Developer-only native multi-Display conformance fixture, never an app dependency.

Geometry and timestamps below are deliberately generated controls. Actual-user-bag
readiness/rendering is covered separately by verify_seek.py. This script does not
claim main-application GUI or independent-per-view visibility acceptance.
"""
import argparse
import base64
import json
import hashlib
from pathlib import Path
import queue
import subprocess
import threading
import uuid

import rosbag
import rospy
import yaml
from geometry_msgs.msg import PoseStamped, TransformStamped
from nav_msgs.msg import Odometry, Path as RosPath
from sensor_msgs.point_cloud2 import create_cloud_xyz32
from std_msgs.msg import Header, String
from tf2_msgs.msg import TFMessage
from visualization_msgs.msg import Marker, MarkerArray


def nodes(root):
    yield root
    for child in root.get("children", []):
        yield from nodes(child)


def displays(tree):
    return [n for n in nodes(tree["displays"]) if n.get("plugin_class")]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--port", type=int, default=49173)
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "source": "generated native multi-Display control fixture", "checks": [],
              "main_application_gui_verified": False, "vanilla_rviz_gui_verified": False,
              "independent_view_visibility_verified": False, "simultaneous_sources_verified": False}
    host = None

    def check(ok, name):
        report["checks"].append({"name": name, "passed": bool(ok)})
        if not ok:
            raise RuntimeError(name)

    try:
        cloud_topic, path_topic, marker_topic, odom_topic = "/arbitrary/research/cloud", "/arbitrary/research/path", "/arbitrary/research/markers", "/arbitrary/research/motion"
        bag_file = output / "native-multi-display-control.bag"
        with rosbag.Bag(str(bag_file), "w") as bag:
            for sec in range(100, 105):
                stamp = rospy.Time(sec)
                header = Header(stamp=stamp, frame_id="world")
                record = rospy.Time(2000 + sec - 100)
                cloud = create_cloud_xyz32(header, [(x / 5, y / 5, (x + y) / 20) for x in range(-10, 11) for y in range(-10, 11)])
                bag.write(cloud_topic, cloud, record)
                path = RosPath(header=header)
                for x in range(-3, 4):
                    pose = PoseStamped(header=header)
                    pose.pose.position.x, pose.pose.position.y = x, 2
                    pose.pose.orientation.w = 1
                    path.poses.append(pose)
                bag.write(path_topic, path, record)
                marker = Marker(header=header, ns="generated-control", id=1, type=Marker.SPHERE, action=Marker.ADD)
                marker.pose.position.y = -2
                marker.pose.orientation.w = 1
                marker.scale.x = marker.scale.y = marker.scale.z = 0.6
                marker.color.r, marker.color.g, marker.color.b, marker.color.a = 1, 0.3, 0.1, 1
                bag.write(marker_topic, MarkerArray([marker]), record)
                odom = Odometry(header=header, child_frame_id="drone")
                odom.pose.pose.orientation.w = 1
                odom.pose.pose.position.x = (sec - 100) / 4
                bag.write(odom_topic, odom, record)
                transform = TransformStamped(header=header, child_frame_id="drone")
                transform.transform.translation.x = (sec - 100) / 4
                transform.transform.rotation.w = 1
                bag.write("/tf", TFMessage([transform]), record)
            bag.write("/unsupported/diagnostic_text", String("untimed control; do not infer an experiment clock"), rospy.Time(2000))
        entries = []
        for i in range(3):
            entries.append({"Class": "rviz/PointCloud2", "Name": "Cloud " + str(i), "Enabled": True, "Topic": cloud_topic,
                            "Color Transformer": "FlatColor", "Color": "80; 170; 255", "Size (Pixels)": i + 3, "Alpha": 0.5 + i / 10})
        entries[0]["Unknown Future Setting"] = {"nested": [1, 2, {"keep": "exact unknown information"}]}
        for i in range(2):
            entries.append({"Class": "rviz/Path", "Name": "Path " + str(i), "Enabled": True, "Topic": path_topic})
        for i in range(4):
            entries.append({"Class": "rviz/MarkerArray", "Name": "Markers " + str(i), "Enabled": True, "Marker Topic": marker_topic})
        entries.extend([{"Class": "rviz/Odometry", "Name": "Motion", "Enabled": True, "Topic": odom_topic},
                        {"Class": "rviz/TF", "Name": "Transforms", "Enabled": True},
                        {"Class": "rviz/Grid", "Name": "Grid", "Enabled": True}])
        missing = {"Class": "uav_unavailable/FutureDisplay", "Name": "Preserved missing plugin", "Enabled": False,
                   "Unknown Payload": {"nested": ["keep", 23, {"value": True}]}}
        config = {"Custom Top Level": {"keep": ["original", "values"]}, "Visualization Manager": {
            "Class": "", "Enabled": True, "Unknown Manager Setting": {"keep": 81},
            "Global Options": {"Fixed Frame": "world", "Background Color": "24; 24; 24", "Frame Rate": 30},
            "Displays": [{"Class": "rviz/Group", "Name": "Group A", "Enabled": True, "Displays": entries[:5]},
                         {"Class": "rviz/Group", "Name": "Group B", "Enabled": True, "Displays": entries[5:]}, missing],
            "Views": {"Current": {"Class": "rviz/TopDownOrtho", "Target Frame": "world", "Scale": 60, "X": 0, "Y": 0, "Angle": 0}}}}
        original = "# Exact generated source comment must survive as imported source text\n" + yaml.safe_dump(config)
        config_file = output / "native-multi-display-input.rviz"
        config_file.write_text(original)
        messages, sequence = queue.Queue(), 0
        with (output / "host.log").open("w") as log:
            host = subprocess.Popen(["bash", args.launcher, "--hidden", "--port", str(args.port), "--session-token", str(uuid.uuid4()),
                                     "--config", str(config_file)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)

            def read():
                for line in host.stdout:
                    try:
                        messages.put(json.loads(line))
                    except ValueError:
                        messages.put({"error": line})
                messages.put({"event": "eof"})

            threading.Thread(target=read, daemon=True).start()

            def next_message():
                result = messages.get(timeout=90)
                if result.get("event") in ("fatal", "eof") or ("error" in result and "request_id" not in result):
                    raise RuntimeError(str(result))
                return result

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

            tree = call("tree")
            loaded = displays(tree)
            check(len([n for n in loaded if n["plugin_class"] != "rviz/Group" and not n["plugin_failed"]]) == 12,
                  "All twelve applicable native Display instances are instantiated")
            check(len({n["instance_id"] for n in loaded}) == len(loaded), "All Display and Group identities are unique")
            missing_display = next(n for n in loaded if n["plugin_failed"])
            check(missing_display["plugin_class"] == missing["Class"], "Unavailable plugin remains an explicit failed native Display")
            cloud = next(n for n in loaded if n["name"] == "Cloud 0")
            cloud_id = cloud["instance_id"]
            check("Unknown Future Setting" in cloud["unrecognized_config_paths"], "Unknown native property path is explicitly reported")
            alpha = next(n for n in nodes(cloud) if n["name"] == "Alpha")
            call("set", property_id=alpha["id"], value=0.31)
            group_b = next(n for n in loaded if n["name"] == "Group B")
            call("move", property_id=cloud["id"], parent_id=group_b["id"])
            moved = next(n for n in displays(call("tree")) if n["instance_id"] == cloud_id)
            check(moved["parent_instance_id"] == group_b["instance_id"], "Cross-Group move preserves identity and changes the actual native parent")
            rejected = request("move", property_id=group_b["id"], parent_id=group_b["id"])
            check(not rejected["ok"], "A native Group cannot be moved into itself")
            duplicated = call("duplicate", property_id=group_b["id"], name="Independent Group Copy")["display"]
            duplicate_ids = {n["instance_id"] for n in nodes(duplicated) if n.get("plugin_class")}
            check(not duplicate_ids.intersection({n["instance_id"] for n in loaded}), "Duplicated Group and descendants receive independent identities")
            call("remove", property_id=duplicated["id"])
            saved_file = output / "native-multi-display-roundtrip.rviz"
            saved = call("save", file=str(saved_file))
            serialized = yaml.safe_load(saved["yaml"])
            all_saved = []

            def walk(entries):
                for entry in entries:
                    all_saved.append(entry)
                    walk(entry.get("Displays", []))

            walk(serialized["Visualization Manager"]["Displays"])
            saved_cloud = next(n for n in all_saved if n.get("UAV Studio", {}).get("Display ID") == cloud_id)
            check(saved_cloud["Unknown Future Setting"] == config["Visualization Manager"]["Displays"][0]["Displays"][0]["Unknown Future Setting"],
                  "Unknown nested map/list property survives native save and Group move")
            check(next(n for n in all_saved if n["Class"] == missing["Class"])["Unknown Payload"] == missing["Unknown Payload"],
                  "Missing plugin retains original nested payload")
            check(serialized["UAV Studio"]["Original Imported YAML"] == original, "Original imported YAML text including comments is retained exactly")
            check(serialized["Custom Top Level"] == config["Custom Top Level"] and serialized["Visualization Manager"]["Unknown Manager Setting"] == {"keep": 81},
                  "Unknown manager and top-level configuration survives")
            reloaded = call("load", file=str(saved_file))
            restored = displays(reloaded)
            check({n["instance_id"] for n in restored} == {n["instance_id"] for n in loaded}, "Native reload restores every original instance without deleted duplicate resurrection")
            restored_cloud = next(n for n in restored if n["instance_id"] == cloud_id)
            restored_alpha = next(n for n in nodes(restored_cloud) if n["name"] == "Alpha")["value"]
            check(abs(restored_alpha - 0.31) < 1e-5, "Native reload preserves one instance's independent edited property")
            other_cloud = next(n for n in restored if n["name"] == "Cloud 1")
            check(abs(next(n for n in nodes(other_cloud) if n["name"] == "Alpha")["value"] - 0.6) < 1e-5, "Same Topic and type sibling retains its independent Alpha")
            catalog = call("bag.open", file=str(bag_file))
            check(any(n["name"] == "/unsupported/diagnostic_text" and n["message_definition_available"] for n in catalog["topics"]),
                  "Unselected untimed topic remains in the complete native bag catalog with its definition")
            check({n["instance_id"] for n in displays(call("tree"))} == {n["instance_id"] for n in restored}, "Opening a bag preserves Display instances and properties")
            call("bag.index", file=str(bag_file), time_basis="header", untimed_policy="reject",
                 topics=[cloud_topic, path_topic, marker_topic, odom_topic, "/tf"])
            result = call("bag.seek", target_ns="102500000000")
            check(result["native_scene_ready"] and result["required_tf_complete"], "All twelve enabled native Displays receive their real control-fixture data and TF")
            frame = call("render_frame", target_ns="102500000000", width=1920, height=1080)
            (output / "native-twelve-displays.png").write_bytes(base64.b64decode(frame.pop("data_base64"), validate=True))
            report["native_output"] = frame
            view_before = call("tree")["views"]
            scale_before = next(n["value"] for n in nodes(view_before) if n["name"] == "Scale")
            pointer = call("pointer", type="wheel", x=0.5, y=0.5, button=0, buttons=0, modifiers=0, wheel_delta=120)
            view_after = call("tree")["views"]
            scale_after = next(n["value"] for n in nodes(view_after) if n["name"] == "Scale")
            check((pointer["handled_by"] == "native-ViewController" or
                   (pointer["handled_by"] == "native-Tool" and pointer.get("tool_class") == "rviz/MoveCamera"))
                  and pointer["clock_ns"] == "102500000000" and scale_before != scale_after,
                  "Pointer wheel uses the installed native ViewController without changing the experiment clock")
            after_camera = call("bag.advance", target_ns="102500000000")
            check(after_camera["native_scene_ready"] and not after_camera["native_scene_reset"] and after_camera["events_delivered_this_request"] == "0",
                  "Native camera interaction preserves Display history and synchronized scene")
            snapshot = call("save")
            restored_snapshot = call("load_yaml", yaml=snapshot["yaml"])
            check({n["instance_id"] for n in displays(restored_snapshot)} == {n["instance_id"] for n in restored},
                  "In-memory project YAML restores all actual native identities without a temporary file")
            # Real native lifecycle transactions, still explicitly generated
            # controls. Never use these as evidence for the main-app GUI.
            group = next(n for n in displays(restored_snapshot) if n["name"] == "Group A")
            child = next(n for n in nodes(group) if n.get("plugin_class") and n["id"] != group["id"])
            before_ids = {n["instance_id"] for n in displays(restored_snapshot)}
            removed = call("remove_many", property_ids=[group["id"], child["id"], child["id"]])
            removed_ids = {n["instance_id"] for n in nodes(group) if n.get("plugin_class")}
            check(set(removed["removed_instance_ids"]) == removed_ids and removed["removed_count"] == len(removed_ids),
                  "Batch removal deduplicates children already covered by a Group and actually destroys every descendant")
            check({n["instance_id"] for n in displays(call("tree"))} == before_ids - removed_ids,
                  "Batch removal leaves unrelated native Display instances intact")
            undone = call("undo")
            check({n["instance_id"] for n in displays(undone)} == before_ids and undone["history"]["can_redo"],
                  "One native undo restores the entire Group transaction with the same persistent identities")
            redone = call("redo")
            check({n["instance_id"] for n in displays(redone)} == before_ids - removed_ids,
                  "One native redo destroys the complete Group transaction again")
            after_delete = call("save")
            reopened = call("load_yaml", yaml=after_delete["yaml"])
            check({n["instance_id"] for n in displays(reopened)} == before_ids - removed_ids,
                  "Saving and reopening does not resurrect removed native Groups or descendants")
            call("load_yaml", yaml=snapshot["yaml"])
            # Settle real TF/Marker geometry before taking the baseline; taking
            # it immediately after load races TFDisplay's first native update.
            call("bag.index", file=str(bag_file), time_basis="header", untimed_policy="reject",
                 topics=[cloud_topic, path_topic, marker_topic, odom_topic, "/tf"])
            call("bag.seek", target_ns="102500000000")
            baseline = call("resources")
            resource_samples = [{"iteration": 0, "resources": baseline}]
            report["lifecycle_resource_samples"] = resource_samples
            for iteration in range(100):
                added = call("add", **{"class": "rviz/PointCloud2", "name": "Lifecycle transient " + str(iteration), "enabled": True})["display"]
                transient_topic = next(n for n in nodes(added) if n["name"] == "Topic")
                call("set", property_id=transient_topic["id"], value=cloud_topic)
                call("remove", property_id=added["id"])
                if iteration in (0, 24, 49, 74, 99):
                    stats = call("resources")
                    resource_samples.append({"iteration": iteration + 1, "resources": stats})
                    check(all(stats[key] == baseline[key] for key in ("displays", "groups", "scene_nodes", "cameras", "movable_objects", "tracked_display_sources")),
                          "Real PointCloud2 add/configure/remove cycle " + str(iteration + 1) + " returns native object counts to baseline")
            report["lifecycle_resource_samples"] = resource_samples
            check(int(call("history")["snapshot_bytes"]) <= 64 * 1024 * 1024,
                  "Native undo snapshots remain within their explicit bounded memory budget after 100 cycles")
            check(not any(n["name"].startswith("Lifecycle transient") for n in displays(call("tree"))),
                  "All 100 transient native Displays are absent from the actual tree")
            bag_hash = hashlib.sha256(bag_file.read_bytes()).hexdigest()
            call("bag.index", file=str(bag_file), time_basis="header", untimed_policy="reject",
                 topics=[cloud_topic, path_topic, marker_topic, odom_topic, "/tf"])
            call("bag.seek", target_ns="102500000000")
            before_close = call("tree")
            call("transform", frame="drone", target_frame="world", time_ns="102500000000")
            closed = call("bag.close")
            stats = closed["resources"]
            check(stats["source_id"] == "none" and not stats["source_available"] and not stats["playback_running"],
                  "Removing the native source clears source identity and stops owned playback")
            check(stats["tf_frames"] == 0 and not stats["output_texture_retained"]
                  and stats["replay"]["history_events"] == "0" and stats["replay"]["owned_observers"] == 0
                  and stats["replay"]["owned_topic_publishers"] == 0 and not stats["replay"]["bag_open"],
                  "Removing the native source releases actual replay history, observers, publishers, bag, TF cache and output texture")
            check({n["instance_id"] for n in displays(closed["tree"])} == {n["instance_id"] for n in displays(before_close)},
                  "Source removal retains every native Display and Group configuration identity")
            closed_file = output / "native-source-removed.rviz"
            closed_snapshot = call("save", file=str(closed_file))
            closed_config = yaml.safe_load(closed_snapshot["yaml"])
            check(closed_config["Visualization Manager"]["Enabled"] and any(n["Enabled"] for n in closed_config["Visualization Manager"]["Displays"]),
                  "Detached native configuration preserves requested Enabled states instead of serializing suspension as user edits")
            no_source = call("load_yaml", yaml=closed_snapshot["yaml"])
            check(not no_source["source_available"] and no_source["source_id"] == "none",
                  "Reopening a detached snapshot never reopens its removed bag source")
            no_tf = request("transform", frame="drone", target_frame="world", time_ns="102500000000")
            check(not no_tf["ok"], "Removed TF frames cannot be looked up after snapshot reload")
            graph = call("resources")
            check(graph["ros_graph_available"] and not {cloud_topic, path_topic, marker_topic, odom_topic}.intersection(graph["ros_subscriber_topics"]),
                  "Actual ROS master confirms removed source Display subscriptions are absent; idle core TF/clock listeners are reported separately")
            no_play = request("bag.play_from_start", rate=1)
            check(not no_play["ok"], "A removed bag cannot resume playback through an old file path")
            # Drain native queued warning updates through a few real tree calls.
            for _ in range(4):
                source_tree = call("tree")
            check(any(n.get("name") == "UAV Studio Source" and n.get("status_level") == 1 for n in nodes(source_tree["displays"])),
                  "Retained native configuration has a real Source Warning rather than fake OK")
            call("bag.open", file=str(bag_file))
            call("bag.play_from_start", rate=1)
            playing_closed = call("bag.close")
            check(not playing_closed["resources"]["playback_running"], "Removing a source during real rosbag playback stops the owned process")
            call("bag.open", file=str(bag_file))
            call("bag.index", file=str(bag_file), time_basis="header", untimed_policy="reject",
                 topics=[cloud_topic, path_topic, marker_topic, odom_topic, "/tf"])
            sequence += 1
            seek_id = sequence
            host.stdin.write(json.dumps(dict(command="bag.seek", request_id=seek_id, target_ns="102500000000")) + "\n")
            sequence += 1
            close_id = sequence
            host.stdin.write(json.dumps(dict(command="bag.close", request_id=close_id)) + "\n")
            host.stdin.flush()
            replies = {}
            while seek_id not in replies or close_id not in replies:
                response = next_message()
                if response.get("request_id") in (seek_id, close_id):
                    replies[response["request_id"]] = response
            check(not replies[seek_id]["ok"] and replies[close_id]["ok"],
                  "Source removal cancels an in-flight real native history job and settles both request identities")
            check(hashlib.sha256(bag_file.read_bytes()).hexdigest() == bag_hash,
                  "Native remove/rebind/play/cancel cycles leave the source bag SHA256 unchanged")
            report["source_removal_resources"] = replies[close_id]["result"]["resources"]
            report["instances"] = [{key: n[key] for key in ("name", "plugin_class", "instance_id", "plugin_failed")} for n in restored]
            report["native_readiness"] = result["native_readiness"]
            call("shutdown")
            host.wait(timeout=20)
            check(host.returncode == 0, "Owned native host and dedicated ROS master shut down normally")
            messages = queue.Queue()
            host = subprocess.Popen(["bash", args.launcher, "--hidden", "--port", str(args.port), "--session-token", str(uuid.uuid4()),
                                     "--config", str(closed_file)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)
            threading.Thread(target=read, daemon=True).start()
            while next_message().get("event") != "ready":
                pass
            fresh = call("tree")
            fresh_resources = call("resources")
            check(fresh["source_id"] == "none" and not fresh["source_available"] and fresh_resources["tf_frames"] == 0,
                  "A fresh native process restores removed-source state without reopening the bag or resurrecting TF")
            check({n["instance_id"] for n in displays(fresh)} == {n["instance_id"] for n in displays(closed["tree"])},
                  "A fresh native process restores retained Display identities after source removal")
            call("shutdown")
            host.wait(timeout=20)
            check(host.returncode == 0, "Restarted native host and its dedicated ROS master shut down normally")
        report["passed"] = True
    finally:
        if host and host.poll() is None:
            try:
                host.stdin.close()
                host.wait(timeout=25)
            except Exception:
                host.terminate()
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({"passed": report["passed"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
