#!/usr/bin/env bash
# 应用运行不需要 Python；roscore 是 ROS 自身提供的进程。
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$script_dir/uav-studio-rviz-host" ]]; then
  host="$script_dir/uav-studio-rviz-host"
else
  host="$(cd -- "$script_dir/../.." && pwd)/build-native/rviz-host/uav-studio-rviz-host"
fi
setup="/opt/ros/noetic/setup.bash"
port=49175
config=""
visible=true
session_token=""
stop_token=""
probe=false
while (($#)); do
  case "$1" in
    --host) host="$2"; shift 2;;
    --setup) setup="$2"; shift 2;;
    --port) port="$2"; shift 2;;
    --config) config="$2"; shift 2;;
    --hidden) visible=false; shift;;
    --session-token) session_token="$2"; shift 2;;
    --stop-session) stop_token="$2"; shift 2;;
    --probe) probe=true; shift;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; exit 2;;
  esac
done
json_string() {
  local value="$1"
  value="${value//\\/\\\\}"; value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"; value="${value//$'\r'/\\r}"; value="${value//$'\t'/\\t}"
  printf '"%s"' "$value"
}
probe_result() {
  printf '{"available":%s,"code":' "$1"; json_string "$2"
  printf ',"message":'; json_string "$3"
  printf ',"host":'; json_string "$host"
  printf ',"setup":'; json_string "$setup"
  printf ',"ros_distribution":"noetic","display":'; json_string "${DISPLAY:-}"
  printf ',"native_optional":true}\n'
}
if [[ "$probe" == true ]]; then
  [[ -f "$host" && -x "$host" ]] || { probe_result false host_missing 'Packaged native RViz host is missing or not executable.'; exit 2; }
  [[ -r "$setup" ]] || { probe_result false ros_missing 'Optional native RViz requires an installed ROS 1 Noetic runtime.'; exit 2; }
  set +u
  if ! source "$setup" >&2; then probe_result false ros_setup_failed 'ROS environment setup failed.'; exit 2; fi
  set -u
  [[ "${ROS_DISTRO:-}" == noetic ]] || { probe_result false ros_incompatible 'The native host requires the ROS 1 Noetic ABI.'; exit 2; }
  for dependency in roscore rospack setsid ldd; do
    command -v "$dependency" >/dev/null || { probe_result false dependency_missing "Missing native runtime command: $dependency"; exit 2; }
  done
  rospack find rviz >/dev/null 2>&1 || { probe_result false rviz_missing 'The optional RViz/librviz package is not installed.'; exit 2; }
  dependencies="$(ldd "$host" 2>&1)" || { probe_result false host_incompatible "$dependencies"; exit 2; }
  if [[ "$dependencies" == *'not found'* ]]; then probe_result false library_missing "$dependencies"; exit 2; fi
  [[ -n "${DISPLAY:-}" ]] || { probe_result false display_missing 'No graphical OpenGL display is available. WSL requires a working WSLg/X display.'; exit 2; }
  probe_result true ready 'Optional native RViz dependencies were detected; OpenGL is verified when the integrated host starts.'
  exit 0
fi
valid_token() { [[ "$1" =~ ^[A-Za-z0-9-]{16,64}$ ]]; }
# A caller can stop only its own token-addressed wrapper, never a broad ROS name.
if [[ -n "$stop_token" ]]; then
  valid_token "$stop_token" || exit 2
  state="/tmp/uav-studio-rviz-session-$UID-$stop_token"
  [[ -f "$state/owner" && "$(stat -c %u "$state/owner")" == "$UID" ]] || exit 0
  read -r owner_pid owner_start <"$state/owner"
  [[ "$owner_pid" =~ ^[0-9]+$ && "$owner_start" =~ ^[0-9]+$ && -r "/proc/$owner_pid/stat" ]] || exit 0
  current_stat="$(cat "/proc/$owner_pid/stat")"; current_stat="${current_stat##*) }"
  read -r -a stat_fields <<<"$current_stat"
  [[ "${stat_fields[19]}" == "$owner_start" ]] || exit 0
  readarray -d '' -t owner_args <"/proc/$owner_pid/cmdline"
  found=false
  for ((i=0;i+1<${#owner_args[@]};i++)); do
    if [[ "${owner_args[i]}" == --session-token && "${owner_args[i+1]}" == "$stop_token" ]]; then found=true; break; fi
  done
  [[ "$found" == true ]] || exit 0
  kill -TERM "$owner_pid"
  exit 0
fi
if [[ "$port" == auto ]]; then
  # Only probe candidates. Never stop another application's ROS master.
  for ((attempt=0;attempt<64;attempt++)); do
    port=$((49152 + RANDOM % 16384))
    if ! (echo >"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then break; fi
  done
fi
[[ "$port" =~ ^[0-9]+$ ]] && ((port>1024&&port<65536)) || { printf 'Invalid dedicated ROS port\n' >&2; exit 2; }
[[ -x "$host" && -r "$setup" ]] || { printf 'Native RViz host or ROS setup not installed\n' >&2; exit 2; }
if (echo >"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
  printf 'Dedicated ROS port is already in use; existing processes remain untouched.\n' >&2
  exit 3
fi
# ROS setup 本身不保证 set -u 兼容。
set +u
source "$setup"
set -u
export ROS_MASTER_URI="http://127.0.0.1:$port"
session="$(mktemp -d "${TMPDIR:-/tmp}/uav-studio-rviz.XXXXXXXX")"
state=""
if [[ -n "$session_token" ]]; then
  valid_token "$session_token" || exit 2
  state="/tmp/uav-studio-rviz-session-$UID-$session_token"
  (umask 077; mkdir "$state") || exit 3
  own_stat="$(cat "/proc/$$/stat")"; own_stat="${own_stat##*) }"
  read -r -a stat_fields <<<"$own_stat"
  printf '%s %s\n' "$$" "${stat_fields[19]}" >"$state/owner"
fi
export ROS_HOME="$session/ros-home" ROS_LOG_DIR="$session/roslogs"
mkdir -p "$ROS_HOME" "$ROS_LOG_DIR"
master_pid=""
host_pid=""
cleanup() {
  trap '' INT TERM HUP
  if [[ -n "$host_pid" ]]; then
    kill -TERM -- "-$host_pid" 2>/dev/null || true
    for ((attempt=0;attempt<20;attempt++)); do
      kill -0 "$host_pid" 2>/dev/null || break
      sleep .1
    done
    kill -KILL -- "-$host_pid" 2>/dev/null || true
    wait "$host_pid" 2>/dev/null || true
    host_pid=""
  fi
  if [[ -n "$master_pid" ]]; then
    kill -INT -- "-$master_pid" 2>/dev/null || true
    for ((attempt=0;attempt<30;attempt++)); do
      kill -0 "$master_pid" 2>/dev/null || break
      sleep .1
    done
    if kill -0 "$master_pid" 2>/dev/null; then
      kill -TERM -- "-$master_pid" 2>/dev/null || true
      for ((attempt=0;attempt<20;attempt++)); do
        kill -0 "$master_pid" 2>/dev/null || break
        sleep .1
      done
      kill -KILL -- "-$master_pid" 2>/dev/null || true
    fi
    wait "$master_pid" 2>/dev/null || true
    master_pid=""
  fi
  # 保留实际日志以便诊断，不执行递归删除。
  printf 'UAV Studio RViz session logs: %s\n' "$session" >&2
  if [[ -n "$state" ]]; then rm -f -- "$state/owner"; rmdir -- "$state" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
setsid roscore -p "$port" >&2 &
master_pid=$!
ready=false
for ((i=0;i<100;i++)); do
  if (echo >"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then ready=true; break; fi
  kill -0 "$master_pid" 2>/dev/null || break
  sleep .1
done
[[ "$ready" == true ]] || { printf 'Dedicated ROS master did not start\n' >&2; exit 4; }
args=(--sim-time)
[[ "$visible" == true ]] || args+=(--hidden)
[[ -z "$config" ]] || args+=("$config")
# Explicit stdin keeps the JSON pipe attached for this asynchronous child. Waiting
# in Bash allows TERM to execute the owned-session cleanup even if OGRE hangs.
setsid "$host" "${args[@]}" <&0 &
host_pid=$!
wait "$host_pid"
