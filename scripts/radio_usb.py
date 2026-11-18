import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time


API_HELP = (
    "请安装并配置 Termux:API Android 应用（与 Termux 来自相同来源/签名），"
    "并在 Termux 中执行 pkg install termux-api python。"
    "解锁手机，允许 Termux:API 的 USB 访问请求，再重试 termux-usb -l。"
)
STOPPING = False
USB_PATH = re.compile(r"^/dev/bus/usb/[0-9]{3}/[0-9]{3}$")
MODEL_NAMES = {"ft891": ("ft891-enhanced", "ft891-standard"), "g90": ("g90",)}


def stopped(signum, frame):
    global STOPPING
    STOPPING = True


def atomic_json(path, value):
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    temporary.write_text(json.dumps(value, ensure_ascii=False) + "\n")
    temporary.replace(path)


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def process_token(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return None if fields[0] == "Z" else fields[19]
    except (OSError, IndexError):
        return None


def alive(record):
    pid = record.get("pid")
    token = record.get("token")
    return isinstance(pid, int) and bool(token) and process_token(pid) == token


def usb_devices():
    if not shutil.which("termux-usb"):
        raise RuntimeError("找不到 termux-usb。" + API_HELP)
    try:
        result = subprocess.run(["termux-usb", "-l"], text=True, capture_output=True, timeout=20)
        if result.returncode:
            raise ValueError(result.stderr.strip() or result.stdout.strip())
        devices = json.loads(result.stdout)
        if not isinstance(devices, list) or any(not isinstance(path, str) or not USB_PATH.fullmatch(path) for path in devices):
            raise ValueError("termux-usb -l 未返回 USB 路径数组")
        return devices
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        raise RuntimeError(f"无法使用 Termux:API：{error}。{API_HELP}") from error


def sysfs_identity(path):
    bus, address = [int(item) for item in path.split("/")[-2:]]
    for device in Path("/sys/bus/usb/devices").glob("*"):
        try:
            if int((device / "busnum").read_text()) != bus or int((device / "devnum").read_text()) != address:
                continue
            return int((device / "idVendor").read_text(), 16), int((device / "idProduct").read_text(), 16)
        except (OSError, ValueError):
            continue
    return None


def compatible(identity):
    return identity == (0x10C4, 0xEA70) or identity in ((0x0403, 0x6001), (0x0403, 0x6015))


def terminate(process, record):
    if alive(record):
        try:
            os.kill(record["pid"], signal.SIGTERM)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


def remove_record(path, links):
    record = read_json(path)
    if alive(record):
        try:
            os.kill(record["pid"], signal.SIGTERM)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + 5
        while alive(record) and time.monotonic() < deadline:
            time.sleep(0.1)
        if alive(record):
            raise RuntimeError("旧桥接进程未能停止，避免同时打开同一 USB 设备。")
    for name, target in record.get("ports", {}).items():
        if name not in MODEL_NAMES.get(record.get("model"), ()):
            continue
        link = links / name
        if link.is_symlink() and os.readlink(link) == target:
            link.unlink()
    path.unlink(missing_ok=True)


def snapshot(state, links):
    supervisor = read_json(state / "supervisor.json")
    devices = []
    if alive(supervisor):
        for path in sorted(state.glob("device-*.json")):
            value = read_json(path)
            if value.get("ready") and alive(value):
                expected = MODEL_NAMES.get(value.get("model"), ())
                ports = value.get("ports", {})
                if expected and all(name in ports and Path(ports[name]).exists() for name in expected):
                    devices.append(value)
    return {"running": alive(supervisor), "ready": bool(devices), "devices": devices,
            "settings": {name: supervisor.get(name) for name in ("baud", "baud_mode", "radio")},
            "link_dir": str(links), "log": str(state / "service.log"),
            "error": read_json(state / "error.json").get("error", "")}


def callback(bridge, arguments, path):
    command = shlex.join([str(bridge), *arguments])
    return ["termux-usb", "-r", "-e", command, path]


def wait_report(process, path, timeout):
    deadline = time.monotonic() + timeout
    while not STOPPING and time.monotonic() < deadline:
        value = read_json(path)
        if value:
            return value
        if process.poll() is not None:
            return {}
        time.sleep(0.1)
    return {}


def serve(args, state, links, bridge):
    lock = (state / "service.lock").open("a")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return 0
    signal.signal(signal.SIGTERM, stopped)
    signal.signal(signal.SIGINT, stopped)
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    for path in state.glob("device-*.json"):
        remove_record(path, links)
    atomic_json(state / "supervisor.json", {"pid": os.getpid(), "token": process_token(os.getpid()),
                                          "baud": args.baud, "baud_mode": args.baud_mode, "radio": args.radio})
    children = {}
    examined = {}
    announced_api_error = ""
    have_wake_lock = False
    try:
        if shutil.which("termux-wake-lock"):
            have_wake_lock = subprocess.run(["termux-wake-lock"], timeout=10).returncode == 0
        while not STOPPING:
            try:
                paths = usb_devices()
                announced_api_error = ""
                (state / "error.json").unlink(missing_ok=True)
            except RuntimeError as error:
                if str(error) != announced_api_error:
                    print(error, file=sys.stderr, flush=True)
                    announced_api_error = str(error)
                    atomic_json(state / "error.json", {"error": str(error)})
                paths = None
            for path, child in list(children.items()):
                process, record, ready_file = child
                if process.poll() is not None or not alive(record) or (paths is not None and path not in paths):
                    terminate(process, record)
                    ready_file.unlink(missing_ok=True)
                    for name, target in record.get("ports", {}).items():
                        link = links / name
                        if link.is_symlink() and os.readlink(link) == target:
                            link.unlink()
                    del children[path]
                    examined[path] = time.monotonic() - 55
            if paths is not None:
                examined = {path: stamp for path, stamp in examined.items() if path in paths}
                for path in paths:
                    if STOPPING:
                        break
                    if path in children or time.monotonic() - examined.get(path, -1000) < 60:
                        continue
                    examined[path] = time.monotonic()
                    identity = sysfs_identity(path)
                    if identity is not None and not compatible(identity):
                        print(f"跳过 {path}: {identity[0]:04x}:{identity[1]:04x}", flush=True)
                        continue
                    suffix = path.replace("/", "-")
                    probe_file = state / f"probe{suffix}.json"
                    probe_file.unlink(missing_ok=True)
                    print(f"检查 {path}；如 Android 提示，请允许 USB 访问。", flush=True)
                    with (state / "service.log").open("a") as output:
                        process = subprocess.Popen(callback(bridge, ["--probe", "--report", str(probe_file)], path),
                                                   stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                                   start_new_session=True)
                        identity_report = wait_report(process, probe_file, 40)
                        terminate(process, identity_report)
                    probe_file.unlink(missing_ok=True)
                    model = identity_report.get("model")
                    print(f"{path}: {json.dumps(identity_report, ensure_ascii=False)}", flush=True)
                    if identity_report and model not in MODEL_NAMES:
                        examined[path] = float("inf")
                    if model not in MODEL_NAMES or args.radio not in ("auto", model):
                        continue
                    if any(child[1].get("model") == model for child in children.values()):
                        print(f"已有 {model}；忽略同类第二个适配器 {path}，避免覆盖固定路径。", flush=True)
                        continue
                    ready_file = state / f"device-{model}.json"
                    ready_file.unlink(missing_ok=True)
                    arguments = ["--radio", model, "--link-dir", str(links), "--report", str(ready_file),
                                 "--baud", str(args.baud), "--baud-mode", args.baud_mode,
                                 "--enhanced", str(args.enhanced), "--standard", str(args.standard)]
                    with (state / "service.log").open("a") as output:
                        process = subprocess.Popen(callback(bridge, arguments, path), stdin=subprocess.DEVNULL,
                                                   stdout=output, stderr=output, start_new_session=True)
                    record = wait_report(process, ready_file, 40)
                    record["token"] = process_token(record.get("pid", -1))
                    record["usb_path"] = path
                    if not record.get("ready") or not alive(record) or process.poll() is not None:
                        terminate(process, record)
                        ready_file.unlink(missing_ok=True)
                        print(f"{path}: 桥接失败，检查 service.log。", flush=True)
                        continue
                    atomic_json(ready_file, record)
                    children[path] = (process, record, ready_file)
                    print(f"{model} 兼容适配器已就绪：{record['ports']}", flush=True)
            deadline = time.monotonic() + 2
            while not STOPPING and time.monotonic() < deadline:
                time.sleep(0.2)
    finally:
        for process, record, ready_file in children.values():
            terminate(process, record)
            ready_file.unlink(missing_ok=True)
        (state / "supervisor.json").unlink(missing_ok=True)
        if have_wake_lock:
            subprocess.run(["termux-wake-unlock"], timeout=10)
        lock.close()
    return 0


def parser():
    result = argparse.ArgumentParser(description="Termux USB 电台串口后台服务；拔线后等待重连，stop 才主动停止。")
    result.add_argument("action", choices=("start", "status", "stop", "__serve"), nargs="?", default="start")
    result.add_argument("--baud", type=int, default=os.environ.get("RADIO_BAUD", "4800"))
    result.add_argument("--baud-mode", choices=("follow", "fixed"), default=os.environ.get("RADIO_BAUD_MODE", "follow"))
    result.add_argument("--radio", choices=("auto", "ft891", "g90"), default="auto")
    result.add_argument("--enhanced", type=int, default=0)
    result.add_argument("--standard", type=int, default=1)
    result.add_argument("--wait", type=int, default=50, help="start 等待就绪的秒数；超时不停止后台服务")
    result.add_argument("--json", action="store_true")
    return result


def main():
    os.umask(0o077)
    args = parser().parse_args()
    if args.baud not in (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400):
        raise RuntimeError("不支持的波特率")
    if args.baud_mode not in ("follow", "fixed"):
        raise RuntimeError("RADIO_BAUD_MODE 必须为 follow 或 fixed")
    state = Path(os.environ.get("RADIO_STATE_DIR", str(Path.home() / ".local/state/termux-radio"))).resolve()
    links = Path(os.environ.get("RADIO_LINK_DIR", str(Path.home() / ".local/run/termux-radio"))).resolve()
    state.mkdir(parents=True, exist_ok=True)
    links.mkdir(parents=True, exist_ok=True)
    bridge = Path(os.environ.get("RADIO_BRIDGE", shutil.which("radio-bridge") or str(Path(__file__).resolve().parent.parent / "radio-bridge"))).resolve()
    if args.action == "__serve":
        return serve(args, state, links, bridge)
    if args.action == "stop":
        supervisor = read_json(state / "supervisor.json")
        if alive(supervisor):
            os.kill(supervisor["pid"], signal.SIGTERM)
            deadline = time.monotonic() + 45
            while alive(supervisor) and time.monotonic() < deadline:
                time.sleep(0.1)
            if alive(supervisor):
                raise RuntimeError("停止尚未完成，请检查 service.log 后重试 status")
        for path in state.glob("device-*.json"):
            remove_record(path, links)
        print("电台 USB 后台服务已停止。")
        return 0
    if args.action == "start":
        if not bridge.is_file() or not os.access(bridge, os.X_OK):
            raise RuntimeError("找不到 radio-bridge；请先 make && make install，或设置 RADIO_BRIDGE")
        usb_devices()
        current = snapshot(state, links)
        if current["running"] and current["settings"] != {"baud": args.baud, "baud_mode": args.baud_mode, "radio": args.radio}:
            print("已运行的服务保留原配置；如需更改，请先 termux-radio stop，再 start。", file=sys.stderr)
        if not current["running"]:
            arguments = [sys.executable, str(Path(__file__).resolve()), "__serve", "--baud", str(args.baud),
                         "--baud-mode", args.baud_mode, "--radio", args.radio,
                         "--enhanced", str(args.enhanced), "--standard", str(args.standard)]
            with (state / "service.log").open("a") as output:
                subprocess.Popen(arguments, stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                 start_new_session=True)
        deadline = time.monotonic() + max(0, args.wait)
        while time.monotonic() < deadline:
            if snapshot(state, links)["ready"]:
                break
            time.sleep(0.2)
    current = snapshot(state, links)
    if args.json:
        print(json.dumps(current, ensure_ascii=False))
    else:
        print(json.dumps(current, ensure_ascii=False, indent=2))
        if current["running"] and not current["ready"]:
            print("服务继续等待插入电台。解锁手机并允许 USB 授权；日志：" + current["log"])
    return 0 if args.action == "status" or current["ready"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
