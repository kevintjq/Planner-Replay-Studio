"""Developer-only native RViz integration acceptance; never an application dependency."""
import argparse
import base64
import json
import os
from pathlib import Path
import queue
import signal
import socket
import subprocess
import threading
import time


def nodes(root):
    yield root
    for child in root.get("children", []):
        yield from nodes(child)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--bag", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--port", type=int, default=49174)
    args = parser.parse_args()
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    sock = socket.socket()
    occupied = sock.connect_ex(("127.0.0.1", args.port)) == 0
    sock.close()
    if occupied:
        raise RuntimeError("Acceptance port already occupied; no process was modified.")
    env = os.environ.copy()
    env.update(ROS_MASTER_URI=f"http://127.0.0.1:{args.port}",
               ROS_HOME=str(root / "ros-home"), ROS_LOG_DIR=str(root / "roslogs"),
               QT_QPA_PLATFORM="xcb")
    processes = []
    report = {"passed": False, "checks": [], "scene_equivalence_verified": False,
              "main_app_integration_verified": False}
    messages = queue.Queue()
    events = []
    sequence = 0

    def check(condition, message):
        report["checks"].append({"name": message, "passed": bool(condition)})
        if not condition:
            raise RuntimeError(message)

    try:
        with (root / "roscore.log").open("w") as master_log, (root / "host.log").open("w") as host_log:
            master = subprocess.Popen(["roscore", "-p", str(args.port)], env=env,
                                      stdout=master_log, stderr=subprocess.STDOUT,
                                      start_new_session=True)
            processes.append(master)
            for _ in range(100):
                sock = socket.socket()
                ready = sock.connect_ex(("127.0.0.1", args.port)) == 0
                sock.close()
                if ready or master.poll() is not None:
                    break
                time.sleep(.1)
            check(ready, "Isolated ROS master started")
            host = subprocess.Popen([args.host, "--sim-time", args.config], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=host_log, text=True,
                                    env=env, start_new_session=True, bufsize=1)
            processes.append(host)

            def read():
                for line in host.stdout:
                    try:
                        messages.put(json.loads(line))
                    except ValueError:
                        messages.put({"invalid_protocol_line": line})
                messages.put({"process_eof": host.poll()})

            threading.Thread(target=read, daemon=True).start()

            def next_message(timeout=40):
                message = messages.get(timeout=timeout)
                if "invalid_protocol_line" in message or "process_eof" in message or message.get("event") == "fatal":
                    raise RuntimeError(str(message))
                return message

            def request(command, **values):
                nonlocal sequence
                sequence += 1
                host.stdin.write(json.dumps(dict(request_id=sequence, command=command, **values)) + "\n")
                host.stdin.flush()
                while True:
                    message = next_message()
                    if message.get("request_id") == sequence:
                        return message
                    events.append(message)

            def call(command, **values):
                result = request(command, **values)
                check(result.get("ok"), "Native command: " + command + (": " + result.get("error", "") if not result.get("ok") else ""))
                return result["result"]

            message = next_message()
            while message.get("event") != "ready":
                events.append(message)
                message = next_message()
            report["ready"] = message
            check(message["backend"] == "ROS1/Noetic/librviz", "Native librviz host ready")
            plugins = call("plugins")
            (root / "plugins.json").write_text(json.dumps(plugins, ensure_ascii=False, indent=2))
            check(len(plugins["displays"]) >= 20 and len(plugins["views"]) >= 5 and len(plugins["tools"]) >= 5,
                  "Actual installed display/view/tool plugin discovery")
            tree = call("tree", include_options=True)
            (root / "initial-tree.json").write_text(json.dumps(tree, ensure_ascii=False, indent=2))
            report["initial_property_count"] = sum(1 for _ in nodes(tree["displays"]))
            check(report["initial_property_count"] > 100, "Real configured native property tree")
            unknown = request("set", property_id="expired", value=1)
            check(not unknown["ok"], "Expired property identifiers rejected")
            added = call("add", **{"class": "rviz/PointCloud2", "name": "Host Acceptance Cloud", "enabled": False})["display"]
            alpha = next(n for n in nodes(added) if n["name"] == "Alpha")
            check(not request("set", property_id=alpha["id"], value=.42)["ok"], "Native disabled child rejects editing")
            call("set", property_id=added["id"], value=True)
            call("set", property_id=alpha["id"], value=.42)
            duplicate = call("duplicate", property_id=added["id"], name="Host Acceptance Copy")["display"]
            duplicate_alpha = next(n for n in nodes(duplicate) if n["name"] == "Alpha")
            check(abs(duplicate_alpha["value"] - .42) < 1e-5, "Duplicate preserves actual plugin properties")
            call("set", property_id=duplicate_alpha["id"], value=.73)
            tree = call("tree", include_options=True)
            original = next(n for n in nodes(tree["displays"]) if n["id"] == alpha["id"])
            check(abs(original["value"] - .42) < 1e-5, "Same plugin duplicate retains independent alpha")
            style = next(n for n in nodes(duplicate) if n["name"] == "Style")
            choices = call("options", property_id=style["id"])["options"]
            check(len(choices) >= 4, "Options queried from native editor/requestOptions")
            call("set", property_id=style["id"], value=choices[-1])
            rejected = request("set", property_id=style["id"], value="No Such Style")
            check(not rejected["ok"], "Unknown fixed enum rejected")
            color = next((n for n in nodes(tree["displays"]) if n["native_type"] == "rviz::ColorProperty" and n["editable"]), None)
            check(color is not None, "Native editable color discovered")
            call("set", property_id=color["id"], value="#345678")
            saved = call("save", file=str(root / "host-roundtrip.rviz"))
            check("Host Acceptance Copy" in saved["yaml"] and "0.73" in saved["yaml"], "Native writer saves real edits")
            call("load", file=str(root / "host-roundtrip.rviz"))
            stale = request("set", property_id=alpha["id"], value=.1)
            check(not stale["ok"], "Reload invalidates deleted display properties")
            # Restore the user's config before the actual recording; acceptance displays never alter it.
            call("load", file=args.config)
            call("capture", file=str(root / "before-bag.png"))
            metadata = call("bag.open", file=args.bag)
            report["bag"] = metadata
            check(int(metadata["message_count"]) > 0 and len(metadata["topics"]) > 0, "Actual bag connection and time metadata")
            call("bag.play_from_start", duration=8, rate=1)
            deadline = time.monotonic() + 18
            finished = False
            while time.monotonic() < deadline:
                try:
                    event = next_message(timeout=1)
                except queue.Empty:
                    continue
                events.append(event)
                if event.get("event") == "bag_playback_finished":
                    check(event["exit_code"] == 0, "Actual rosbag playback completed")
                    finished = True
                    break
            check(finished, "Actual recording played with ROS simulated clock")
            time.sleep(1)
            after = call("tree", include_options=True)
            (root / "after-bag-tree.json").write_text(json.dumps(after, ensure_ascii=False, indent=2))
            call("capture", file=str(root / "after-bag.png"))
            call("window", visible=False)
            preview = call("frame", max_width=640)
            check(preview.get("width") == 640 and preview.get("height") == 360 and not preview.get("high_resolution_export"), "Hidden native framebuffer preview dimensions")
            (root / "hidden-preview.png").write_bytes(base64.b64decode(preview["data_base64"]))
            report["preview_elapsed_ms"] = preview["elapsed_ms"]
            report["preview_clock_ns"] = preview["clock_ns"]
            call("window", visible=True)
            statuses = [n for n in nodes(after["displays"]) if "status" in n]
            report["native_statuses"] = [{k: n[k] for k in ("path", "name", "status", "value")} for n in statuses]
            report["bag_played"] = True
            report["native_data_status_verified"] = any("points" in str(n.get("value", "")).lower() or "messages received" in str(n.get("value", "")).lower() for n in statuses)
            call("shutdown")
            host.wait(timeout=10)
            report["passed"] = True
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGINT)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=5)
        report["events"] = events
        (root / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
        print(json.dumps({k: v for k, v in report.items() if k not in ("events", "native_statuses", "checks")}, ensure_ascii=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
