"""Developer-only live comparison session with the unmodified rviz executable.

Owns an isolated master through run_host.sh. GUI observations are deliberately
not inferred from a successful launch. Stop only this session by creating its
output/stop file, or it automatically expires after twenty minutes.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import threading
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--bag", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--port", type=int, default=49171)
    parser.add_argument("--duration-seconds", type=int, default=300)
    args = parser.parse_args()
    if not 30 <= args.duration_seconds <= 1200:
        parser.error("--duration-seconds must be between 30 and 1200")
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    stop_file = output / "stop"
    if stop_file.exists():
        raise RuntimeError("Previous comparison stop marker exists; use a new output directory")
    config = Path(args.config).resolve()
    report = {"vanilla_gui_observed": False, "pixel_equivalence_certified": False,
              "config_sha256_before": hashlib.sha256(config.read_bytes()).hexdigest()}
    host = stock = None
    messages, sequence = queue.Queue(), 0
    try:
        with (output / "host.log").open("w") as host_log, (output / "stock-rviz.log").open("w") as stock_log:
            host = subprocess.Popen(["bash", args.launcher, "--hidden", "--port", str(args.port), "--session-token", str(uuid.uuid4()),
                "--config", str(config)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=host_log, text=True, bufsize=1)

            def read():
                for line in host.stdout:
                    try:
                        messages.put(json.loads(line))
                    except ValueError:
                        messages.put({"event": "fatal", "error": line})
                messages.put({"event": "eof"})

            threading.Thread(target=read, daemon=True).start()

            def next_message():
                value = messages.get(timeout=90)
                if value.get("event") in ("fatal", "eof"):
                    raise RuntimeError(str(value))
                return value

            while next_message().get("event") != "ready":
                pass

            def call(command, **values):
                nonlocal sequence
                sequence += 1
                host.stdin.write(json.dumps(dict(command=command, request_id=sequence, **values)) + "\n")
                host.stdin.flush()
                while True:
                    value = next_message()
                    if value.get("request_id") == sequence:
                        if not value.get("ok"):
                            raise RuntimeError(str(value))
                        return value["result"]

            env = os.environ.copy()
            # This installed RViz advisory explicitly documents this variable.
            # Avoid its modal blocking the standard renderer before replay;
            # retain the actual ROS/RViz version in the comparison evidence.
            env.update(ROS_MASTER_URI="http://127.0.0.1:" + str(args.port), QT_QPA_PLATFORM="xcb", ROS_LOG_DIR=str(output / "stock-logs"),
                       DISABLE_ROS1_EOL_WARNINGS="1")
            report["stock_eol_advisory_suppressed"] = True
            stock = subprocess.Popen(["rviz", "-d", str(config)], env=env, stdout=stock_log, stderr=subprocess.STDOUT, start_new_session=True)
            time.sleep(3)
            if stock.poll() is not None:
                raise RuntimeError("Unmodified rviz exited during startup: " + str(stock.returncode))
            index = call("bag.index", file=args.bag, time_basis="header", untimed_policy="reject", topics=["/cloud_registered", "/Odometry", "/tf"])
            target = str(int(index["begin_ns"]) + 8_000_000_000)
            report["native_replay"] = call("bag.seek", target_ns=target)
            frame = call("render_frame", target_ns=target, width=1920, height=1080)
            (output / "native-same-config.png").write_bytes(base64.b64decode(frame.pop("data_base64"), validate=True))
            report["native_output"] = frame
            report["native_tree"] = call("tree", include_options=True)
            report["stock_pid"] = stock.pid
            report["native_resources"] = call("resources")
            report["config_sha256_after"] = hashlib.sha256(config.read_bytes()).hexdigest()
            (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))
            print(json.dumps({"ready_for_gui_observation": True, "stock_pid": stock.pid, "target_ns": target, "stop_file": str(stop_file)}), flush=True)
            deadline = time.monotonic() + args.duration_seconds
            while not stop_file.exists() and time.monotonic() < deadline and stock.poll() is None:
                time.sleep(0.1)
            call("shutdown")
            host.wait(timeout=20)
            report["host_exit_code"] = host.returncode
    finally:
        if stock and stock.poll() is None:
            os.killpg(stock.pid, signal.SIGINT)
            try:
                stock.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(stock.pid, signal.SIGTERM)
                stock.wait(timeout=8)
        if host and host.poll() is None:
            host.stdin.close()
            try:
                host.wait(timeout=25)
            except subprocess.TimeoutExpired:
                host.terminate()
        report["config_sha256_after"] = hashlib.sha256(config.read_bytes()).hexdigest()
        if stock:
            report["stock_exit_code"] = stock.returncode
        report["source_configuration_unchanged"] = report["config_sha256_after"] == report["config_sha256_before"]
        (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
