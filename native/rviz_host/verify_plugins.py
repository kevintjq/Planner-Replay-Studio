"""Developer-only installed-plugin contract probe, not a rendering certificate.

Uses the actual librviz factory/property model, never a hardcoded property table.
No generated sensor messages are supplied here. Real recording/rendering evidence
is separate and must not be inferred from successful construction or YAML load.
"""
import argparse
import json
from pathlib import Path
import queue
import subprocess
import threading
import uuid
import base64

import yaml


def nodes(root):
    yield root
    for child in root.get("children", []):
        yield from nodes(child)


def verify_empty_views(call, request, output, config):
    """Real-host YAML regression: empty lists/null are not arbitrary scalars."""
    results = []
    saved = call("save")["yaml"]
    (output / "program-empty-snapshot.rviz").write_text(saved)
    response = request("load_yaml", yaml=saved)
    results.append({"name": "program-written empty camera snapshot roundtrip", "expected": True,
                    "accepted": response.get("ok", False), "error": response.get("error", ""),
                    "passed": response.get("ok", False)})
    base = yaml.safe_load(config.read_text())
    # Restore real controller instances before every case so rejection also
    # proves the current cameras and scene were not destroyed by validation.
    views = [{"view_id": "validation-orbit", "class": "rviz/Orbit"},
             {"view_id": "validation-top", "class": "rviz/TopDownOrtho"}]
    cases = [("missing", None, True), ("empty-list", [], True), ("yaml-null", None, True),
             ("empty-string", "", False), ("list-as-string", "[]", False),
             ("null-as-string", "null", False), ("text", "invalid", False),
             ("integer", 17, False), ("zero", 0, False), ("boolean", False, False),
             ("mapping", {}, False), ("invalid-list-member", ["invalid"], False)]
    for name, value, expected in cases:
        call("views.sync", views=views)
        before = call("tree")
        resources = call("resources")
        candidate = dict(base)
        candidate["UAV Studio"] = {} if name == "missing" else {"Render Views": value}
        response = request("load_yaml", yaml=yaml.safe_dump(candidate))
        accepted = response.get("ok", False)
        after = call("tree")
        unchanged = True
        if not expected:
            current = call("resources")
            unchanged = all(after[key] == before[key] for key in ("scene_id", "source_id", "scene_revision", "render_views"))
            unchanged = unchanged and all(current[key] == resources[key] for key in
                ("displays", "groups", "scene_nodes", "cameras", "movable_objects", "additional_views"))
        empty = not after.get("render_views") if expected and accepted else True
        results.append({"name": name, "expected": expected, "accepted": accepted,
                        "error": response.get("error", ""), "invalid_input_preserved_live_scene": unchanged,
                        "passed": accepted == expected and unchanged and empty})
    call("views.sync", views=[])
    (output / "empty-view-configuration.json").write_text(json.dumps(results, ensure_ascii=False, indent=2))
    return results


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--port", type=int, default=49173)
    parser.add_argument("--bag")
    parser.add_argument("--view-config-only", action="store_true")
    args = parser.parse_args()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"completed": False, "source": "installed librviz plugin factory; no sensor fixture",
              "sensor_rendering_certified": False, "main_application_gui_verified": False,
              "vanilla_rviz_gui_verified": False, "plugins": []}
    for command, field in ((["rosversion", "rviz"], "rviz_version"), (["rosversion", "-d"], "ros_distribution")):
        result = subprocess.run(command, text=True, capture_output=True, check=True)
        report[field] = result.stdout.strip()
    config = output / "empty-native-scene.rviz"
    config.write_text(yaml.safe_dump({"Visualization Manager": {"Class": "", "Enabled": True, "Displays": [],
        "Global Options": {"Fixed Frame": "world", "Background Color": "38; 40; 44"},
        "Views": {"Current": {"Class": "rviz/Orbit", "Target Frame": "world"}}}}))
    host = None
    messages, sequence = queue.Queue(), 0
    try:
        with (output / "host.log").open("w") as log:
            host = subprocess.Popen(["bash", args.launcher, "--hidden", "--port", str(args.port),
                "--session-token", str(uuid.uuid4()), "--config", str(config)], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1)

            def read():
                for line in host.stdout:
                    try:
                        messages.put(json.loads(line))
                    except ValueError:
                        messages.put({"event": "fatal", "error": line})
                messages.put({"event": "eof"})

            threading.Thread(target=read, daemon=True).start()

            def next_message():
                message = messages.get(timeout=90)
                if message.get("event") in ("fatal", "eof"):
                    raise RuntimeError(str(message))
                return message

            while next_message().get("event") != "ready":
                pass

            def request(command, **values):
                nonlocal sequence
                sequence += 1
                host.stdin.write(json.dumps(dict(request_id=sequence, command=command, **values)) + "\n")
                host.stdin.flush()
                while True:
                    message = next_message()
                    if message.get("request_id") == sequence:
                        return message

            def call(command, **values):
                response = request(command, **values)
                if not response.get("ok"):
                    raise RuntimeError(command + ": " + response.get("error", ""))
                return response["result"]

            def display(tree, identity):
                return next(n for n in nodes(tree["displays"]) if n.get("instance_id") == identity)

            baseline = call("resources")
            discovered = call("plugins")
            report["view_plugins"] = discovered["views"]
            report["tool_plugins"] = discovered["tools"]
            if args.view_config_only:
                report["empty_view_configuration"] = verify_empty_views(call, request, output, config)
                report["empty_view_configuration_passed"] = all(case["passed"] for case in report["empty_view_configuration"])
                discovered["displays"] = []
            for index, plugin in enumerate(discovered["displays"]):
                entry = dict(plugin)
                entry.update(created=False, native_properties=False, yaml_roundtrip=False,
                             removed=False, resource_counts_restored=False,
                             data_received=False, actual_sensor_rendering_verified=False,
                             dynamic_enum_edits=[], errors=[])
                report["plugins"].append(entry)
                identity = None
                try:
                    added = call("add", **{"class": plugin["class"], "name": "Installed plugin probe " + str(index), "enabled": True})["display"]
                    identity = added["instance_id"]
                    entry["created"] = added["plugin_class"] == plugin["class"] and not added["plugin_failed"]
                    tree = call("tree", include_options=True)
                    native = display(tree, identity)
                    properties = list(nodes(native))
                    entry["native_properties"] = bool(properties) and all(n.get("id") and n.get("native_type") for n in properties)
                    entry["property_count"] = len(properties)
                    entry["property_types"] = sorted({n["native_type"] for n in properties})
                    entry["topic_properties"] = [{key: n.get(key) for key in ("name", "message_type", "value")}
                        for n in properties if n["native_type"].endswith("RosTopicProperty")]
                    entry["native_statuses_without_data"] = [{"path": n["path"], "level": n["status_level"], "message": n["display_value"]}
                        for n in properties if "status_level" in n]
                    (output / (plugin["class"].replace("/", "-") + "-properties.json")).write_text(json.dumps(native, ensure_ascii=False, indent=2))
                    # Exercise real native enum editors; no synthetic property
                    # examples and no assumption that every enum changes shape.
                    candidates = [n for n in properties if n.get("options_available") and not n.get("allow_custom")
                        and n.get("editable") and n.get("enabled") and not n.get("hidden") and len(n.get("options", [])) > 1]
                    for candidate in candidates[:3]:
                        alternative = next((v for v in candidate["options"] if v != candidate["value"]), None)
                        if alternative is None:
                            continue
                        before_shape = [(n["path"], n["native_type"], n["hidden"], n["editable"]) for n in nodes(native)]
                        edit = request("set", property_id=candidate["id"], value=alternative)
                        outcome = {"path": candidate["path"], "value": alternative, "accepted": edit.get("ok", False)}
                        if edit.get("ok"):
                            changed = display(call("tree", include_options=True), identity)
                            after_shape = [(n["path"], n["native_type"], n["hidden"], n["editable"]) for n in nodes(changed)]
                            outcome["conditional_property_shape_changed"] = before_shape != after_shape
                            match = next((n for n in nodes(changed) if n["path"] == candidate["path"]), None)
                            outcome["actual_value_matches"] = bool(match and match["value"] == alternative)
                            # RobotModel can start with an empty enum while its
                            # URDF is missing. That native invalid default is not
                            # a valid choice to write back through the editor.
                            if match and candidate["value"] in match.get("options", []):
                                call("set", property_id=match["id"], value=candidate["value"])
                            native = display(call("tree", include_options=True), identity)
                        else:
                            outcome["error"] = edit.get("error")
                        entry["dynamic_enum_edits"].append(outcome)
                    saved = call("save")
                    first = yaml.safe_load(saved["yaml"])["Visualization Manager"]["Displays"]
                    restored = call("load_yaml", yaml=saved["yaml"])
                    loaded = display(restored, identity)
                    again = call("save")
                    second = yaml.safe_load(again["yaml"])["Visualization Manager"]["Displays"]
                    entry["yaml_roundtrip"] = first == second and loaded["plugin_class"] == plugin["class"]
                    if first != second:
                        entry["roundtrip_before"] = first
                        entry["roundtrip_after"] = second
                    call("remove", property_id=loaded["id"])
                    entry["removed"] = not any(n.get("instance_id") == identity for n in nodes(call("tree")["displays"]))
                    stats = call("resources")
                    entry["resource_counts_restored"] = all(stats[key] == baseline[key] for key in
                        ("displays", "groups", "cameras", "scene_nodes", "movable_objects"))
                    entry["resources_after_remove"] = {key: stats[key] for key in ("displays", "groups", "cameras", "scene_nodes", "movable_objects")}
                except Exception as error:
                    entry["errors"].append(str(error))
                    if host.poll() is not None:
                        raise
                    call("load", file=str(config))
                (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
            if args.bag:
                catalog = call("bag.open", file=args.bag)
                report["real_bag_catalog"] = [{key: item.get(key) for key in ("name", "type", "message_definition_available")} for item in catalog["topics"]]
                real_entries, cases = [], []
                for plugin in discovered["displays"]:
                    matching = [topic for topic in catalog["topics"] if topic["type"] in plugin["message_types"]]
                    for topic in matching:
                        name = plugin["class"] + " @ " + topic["name"]
                        real_entries.append({"Class": plugin["class"], "Name": name, "Enabled": False})
                        cases.append({"class": plugin["class"], "topic": topic["name"], "type": topic["type"], "name": name})
                for plugin in ("rviz/TF", "rviz/Axes", "rviz/Grid"):
                    real_entries.append({"Class": plugin, "Name": plugin, "Enabled": False})
                    cases.append({"class": plugin, "topic": None, "type": None, "name": plugin})
                real_config = {"Visualization Manager": {"Class": "", "Enabled": True, "Displays": real_entries,
                    "Global Options": {"Fixed Frame": "camera_init", "Background Color": "38; 40; 44"},
                    "Views": {"Current": {"Class": "rviz/Orbit", "Target Frame": "camera_init", "Distance": 30,
                        "Pitch": 0.6, "Yaw": 0.8}}}}
                real_tree = call("load_yaml", yaml=yaml.safe_dump(real_config))
                for case in cases:
                    native = next(n for n in nodes(real_tree["displays"]) if n.get("plugin_class") == case["class"] and n["name"] == case["name"])
                    case["instance_id"] = native["instance_id"]
                    case["property_id"] = native["id"]
                    if case["topic"]:
                        candidates = [n for n in nodes(native) if n["native_type"].endswith("RosTopicProperty") and n.get("message_type") == case["type"]]
                        if len(candidates) != 1:
                            case["setup_error"] = "Plugin does not expose one unique compatible native RosTopicProperty"
                            continue
                        # Some plugins disable child property flags when their
                        # Display is disabled, so configure while enabled.
                        call("set", property_id=case["property_id"], value=True)
                        call("set", property_id=candidates[0]["id"], value=case["topic"])
                        call("set", property_id=case["property_id"], value=False)
                selected = sorted({case["topic"] for case in cases if case["topic"]} | {"/tf"})
                index = call("bag.index", file=args.bag, time_basis="header", untimed_policy="reject", topics=selected)
                target = str(int(index["begin_ns"]) + 8_000_000_000)
                report["real_recording_cases"] = cases
                for case in cases:
                    case["actual_sensor_rendering_verified"] = False
                    if case.get("setup_error"):
                        continue
                    call("set", property_id=case["property_id"], value=True)
                    replay = call("bag.seek", target_ns=target)
                    case["target_ns"] = target
                    case["native_scene_ready"] = replay["native_scene_ready"]
                    case["readiness"] = replay["native_readiness"]
                    case["message_delivery"] = [item for item in replay.get("topics", []) if item.get("name") == case["topic"]]
                    if replay["native_scene_ready"]:
                        frame = call("render_frame", target_ns=target, width=1920, height=1080)
                        image_path = output / (case["name"].replace("/", "-").replace(" @ ", "--") + "-real.png")
                        image_path.write_bytes(base64.b64decode(frame.pop("data_base64"), validate=True))
                        case["native_output"] = frame
                        case["image_file"] = image_path.name
                        # Pixel/content inspection is a separate explicit gate.
                        case["actual_native_render_completed"] = True
                    call("set", property_id=case["property_id"], value=False)
                    (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
            report["owned_shutdown"] = bool(call("shutdown") is not None)
            host.wait(timeout=20)
            report["owned_shutdown"] = report["owned_shutdown"] and host.returncode == 0
            contract_fields = ("created", "native_properties", "yaml_roundtrip", "removed", "resource_counts_restored")
            report["plugin_contract_passed"] = None if args.view_config_only else all(
                all(entry[field] for field in contract_fields) and not entry["errors"] for entry in report["plugins"])
            report["completed"] = True
    finally:
        if host and host.poll() is None:
            host.stdin.close()
            try:
                host.wait(timeout=25)
            except subprocess.TimeoutExpired:
                host.terminate()
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({"completed": report["completed"], "plugins": len(report["plugins"]),
        "created": sum(n["created"] for n in report["plugins"]),
        "roundtrips": sum(n["yaml_roundtrip"] for n in report["plugins"]),
        "removed": sum(n["removed"] for n in report["plugins"])}))
    if report.get("empty_view_configuration_passed") is False or report.get("plugin_contract_passed") is False:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
