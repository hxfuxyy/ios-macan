#!/usr/bin/env python3
"""Upload the prepared Ubuntu/GNOME rootfs to phone RAM and explicitly launch it."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import termios
import time
import zlib
import tempfile
import subprocess
import base64

from collect_macan_serial import DEVICE, PROMPT, exchange


def software_input_fence_stage():
    """Change only the temporary boot's mask, before its first DRM commit."""
    return """awk '$2=="/" && $3=="rootfs" {found=1} END {exit !found}' /proc/1/mounts
if ! awk '$2=="/sys/kernel/debug" && $3=="debugfs" {found=1} END {exit !found}' /proc/1/mounts; then
    mount -t debugfs -o nosuid,nodev,noexec debugfs /proc/1/root/sys/kernel/debug
fi
mask=/proc/1/root/sys/kernel/debug/dri/0/crtc203/hwfence_features_mask
cat "$mask"
printf '0 0\\n' > "$mask"
printf '1 0\\n' > "$mask"
value=$(cat "$mask")
printf '%s\\n' "$value"
printf '%s\\n' "$value" | grep -Fx 'bit 0: 0'
printf '%s\\n' "$value" | grep -Fx 'bit 1: 0'
echo ACE6T_SOFTWARE_FENCE_MASK_SELECTED_HARDWARE_COMPLETION_PENDING
"""


def rebuilt_display_stage(record):
    """Replace only the unloaded driver in PID 1's verified RAM root."""
    item=record["rebuilt_display"]
    if item["path"]!="usr/local/share/ace6t/msm_drm-trace.ko":
        raise ValueError("Unexpected rebuilt display payload path")
    for key in ("sha256","stock_sha256"):
        value=item[key]
        if not isinstance(value,str) or len(value)!=64 or any(c not in "0123456789abcdef" for c in value):
            raise ValueError("Invalid display checksum")
    return [
        "echo 'ACE6T_REBUILT_DISPLAY_PREFLIGHT: checking PID 1 RAM root'",
        "grep -q '^rootfs / rootfs rw ' /proc/1/mounts",
        "grep -q '6.12.38-android16-5-g844001fb8721-ab14552068-4k' /proc/version",
        "if grep -q '^msm_drm ' /proc/modules; then echo 'Display already loaded; refusing replacement'; exit 1; fi",
        "[ ! -L /proc/1/root/lib/modules/msm_drm.ko ]",
        f"echo '{item['stock_sha256']}  /proc/1/root/lib/modules/msm_drm.ko' | sha256sum -c -",
        f"echo '{item['sha256']}  /{item['path']}' | sha256sum -c -",
        f"cp /{item['path']} /proc/1/root/lib/modules/msm_drm.ace6t-trace.tmp",
        "chmod 0644 /proc/1/root/lib/modules/msm_drm.ace6t-trace.tmp",
        "mv /proc/1/root/lib/modules/msm_drm.ace6t-trace.tmp /proc/1/root/lib/modules/msm_drm.ko",
        f"echo '{item['sha256']}  /proc/1/root/lib/modules/msm_drm.ko' | sha256sum -c -",
        "echo 'ACE6T_REBUILT_DISPLAY_STAGED: initramfs only; module not loaded yet'",
    ]


def write_all(fd, data, timeout=30):
    deadline = time.monotonic() + timeout
    view = memoryview(data)
    while view:
        if time.monotonic() >= deadline:
            raise TimeoutError("USB write stalled")
        if not select.select([], [fd], [], 0.25)[1]:
            continue
        try:
            n = os.write(fd, view)
        except BlockingIOError:
            continue
        if n <= 0:
            raise OSError("USB disconnected during upload")
        view = view[n:]


class Lines:
    def __init__(self, fd):
        self.fd = fd
        self.data = bytearray()

    def line(self, timeout=40):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if b"\n" in self.data:
                line, _, rest = self.data.partition(b"\n")
                self.data = bytearray(rest)
                return bytes(line).rstrip(b"\r")
            if not select.select([self.fd], [], [], 0.25)[0]:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except BlockingIOError:
                continue
            if not chunk:
                raise OSError("USB console disconnected")
            self.data.extend(chunk)
            if len(self.data) > 65536:
                raise OSError("Upload response exceeded bound")
        raise TimeoutError("No upload acknowledgement")


def upload(fd, source, size, crc, sha, command="ubuntu-upload"):
    termios.tcflush(fd, termios.TCIFLUSH)
    write_all(fd, f"{command} {size} {crc:08x} {sha}\n".encode())
    lines = Lines(fd)
    if lines.line() != b"UPLOAD_READY 65536":
        raise OSError("Device refused rootfs upload")
    offset = 0
    started = time.monotonic()
    next_report = 0
    with source.open("rb") as stream:
        while offset < size:
            data = stream.read(min(65536, size-offset))
            if not data:
                raise OSError("Rootfs changed during upload")
            write_all(fd, data)
            offset += len(data)
            if lines.line() != f"UPLOAD_ACK {offset}".encode():
                raise OSError("Unexpected USB upload acknowledgement")
            if time.monotonic() >= next_report:
                print(f"Rootfs in RAM: {offset * 100 // size}% ({offset}/{size} bytes)", flush=True)
                next_report = time.monotonic()+5
    if not lines.line().startswith(b"UPLOAD_OK:"):
        raise OSError("Device rejected rootfs checksum")
    # The next exchange drains the upload's terminating prompt.
    return {"bytes": size, "sha256": sha, "crc32": f"{crc:08x}",
            "seconds": round(time.monotonic()-started, 2)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--device", default=DEVICE)
    p.add_argument("--rootfs", type=Path)
    p.add_argument("--script", type=Path, help="Upload/execute a <=64KiB maintenance script inside the unpacked Ubuntu RAM root. Stop GNOME first.")
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--start", action="store_true", help="Explicitly release the grid and start GNOME after verified unpack.")
    p.add_argument("--rebuilt-display", action="store_true", help="Stage the ABI-checked module bundled by prepare_macan_display_rootfs.py in the unloaded RAM root; enable its traces before starting GNOME. Requires --rootfs --start.")
    p.add_argument("--software-input-fences", action="store_true", help="Use real software fence waits with the rebuilt driver's hardware-disable fix; requires --rebuilt-display.")
    p.add_argument("--display-swte", action="store_true", help="v9: explicitly use Qualcomm watchdog TE instead of panel TE on initial display load. Requires --rootfs --start; skips panel read/ESD checks for this RAM boot.")
    p.add_argument("--display-posted-start", action="store_true", help="Use the captured Android frame_trigger_mode=2 in real Weston atomic requests, retaining normal panel TE. Requires --rootfs --start; installs RAM driver snapshots.")
    p.add_argument("--status", action="store_true", help="Read current Ubuntu status/log only.")
    p.add_argument("--stop", action="store_true", help="Explicitly stop only the temporary Ubuntu helper process group.")
    args = p.parse_args()
    if args.rebuilt_display and (not args.rootfs or not args.start or args.status or args.stop):
        p.error("--rebuilt-display requires --rootfs --start")
    if args.software_input_fences and not args.rebuilt_display:
        p.error("--software-input-fences requires --rebuilt-display")
    if args.display_swte and (not args.rootfs or not args.start or args.status or args.stop):
        p.error("--display-swte requires an initial --rootfs --start")
    if args.display_posted_start and (not args.rootfs or not args.start or args.status or args.stop or args.display_swte):
        p.error("--display-posted-start requires --rootfs --start without --display-swte")
    if args.out.exists():
        p.error("Output exists; choose a new --out")
    if args.status or args.stop:
        if args.rootfs or args.script or args.start or (args.status and args.stop):
            p.error("--status and --stop are separate from upload/start")
    elif (args.rootfs is None)==(args.script is None):
        p.error("Specify either the prepared --rootfs or a RAM --script")
    size=crc=0;sha=None
    source=args.rootfs or args.script
    if source:
        size=source.stat().st_size
        if not 0 < size <= (65536 if args.script else 512*1024*1024):
            p.error("Prepared rootfs must be at most 512 MiB compressed")
        digest=hashlib.sha256()
        with source.open("rb") as stream:
            while data:=stream.read(1024*1024):
                digest.update(data);crc=zlib.crc32(data,crc)
        sha=digest.hexdigest()
        if args.rootfs:
            record=json.loads(args.rootfs.with_suffix(".manifest.json").read_text())
            if record["sha256"]!=sha or record["bytes"]!=size or record["desktop"]!="GNOME":
                p.error("Rootfs does not match the prepared GNOME manifest")
            if args.rebuilt_display:
                rebuilt_display_stage(record) # Validate before any USB operation.
            if args.software_input_fences:
                patch = Path(__file__).resolve().parents[1]/"LinuxRamTest/ace6t-display-software-input-fence.patch"
                if record["rebuilt_display"].get("software_input_fence_patch_sha256") != hashlib.sha256(patch.read_bytes()).hexdigest():
                    p.error("Rootfs must contain the checked software input-fence fix")
    fd=os.open(args.device,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
    old=termios.tcgetattr(fd)
    args.out.mkdir(parents=True)
    manifest={"device":args.device,"files":{},"start_requested":args.start,
              "display_swte_requested":args.display_swte,
              "display_posted_start_requested":args.display_posted_start,
              "rebuilt_display_requested":args.rebuilt_display}
    manifest["software_input_fences_requested"] = args.software_input_fences
    def capture(command):
        data,complete=exchange(fd,command,timeout=90)
        name=f"{len(manifest['files']):02d}-{command}.txt"
        (args.out/name).write_bytes(data)
        manifest["files"][name]={"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"prompt":complete}
        if not complete:
            raise TimeoutError(f"No prompt after {command}")
        print(f"{command}: {len(data)} bytes",flush=True)
        return data
    try:
        fcntl.ioctl(fd,termios.TIOCEXCL)
        raw=termios.tcgetattr(fd);raw[0]=raw[1]=raw[3]=0
        raw[2]=termios.CS8|termios.CREAD|termios.CLOCAL
        raw[6][termios.VMIN]=raw[6][termios.VTIME]=0
        termios.tcsetattr(fd,termios.TCSANOW,raw)
        time.sleep(1)
        help_text=capture("help")
        if b"ubuntu-upload" not in help_text:
            raise OSError("This image lacks the Ubuntu RAM helper")
        if args.rebuilt_display and b"ubuntu-maintain" not in help_text:
            raise OSError("Rebuilt display staging requires RAM maintenance; no payload sent")
        if args.display_swte and b"display-swte" not in help_text.split():
            raise OSError("This image lacks the explicit display-swte command; no payload sent")
        if args.stop:
            capture("ubuntu-stop");time.sleep(2)
        elif source:
            if args.script and b"ubuntu-maintain" not in help_text:
                raise OSError("This image lacks live RAM maintenance")
            manifest["upload"]=upload(fd,source,size,crc,sha,"ubuntu-script" if args.script else "ubuntu-upload")
            capture("ubuntu-maintain" if args.script else "ubuntu-unpack")
            deadline=time.monotonic()+180
            while True:
                time.sleep(2)
                status=capture("ubuntu-status")
                if b"unpacked=1" in status and b"child=0" in status and b"wait-status=0" in status:
                    break
                if b"child=0" in status or time.monotonic()>deadline:
                    capture("ubuntu-log")
                    operation = "RAM maintenance" if args.script else "unpack"
                    raise OSError(f"Ubuntu {operation} failed or timed out; retained logs")
            if args.start:
                if args.rootfs and b"ubuntu-maintain" in help_text:
                    # Refresh only the two GNOME launch scripts in the RAM root;
                    # this avoids rebuilding/resending a 475MiB archive for fixes.
                    scripts=Path(__file__).resolve().parents[1]/"LinuxRamTest"
                    lines=["#!/bin/sh", "set -eu"]
                    if args.rebuilt_display:
                        lines += rebuilt_display_stage(record)
                    for name,target in [("ace6t-gnome.sh","ace6t-gnome"),
                                        ("ace6t-gnome-session.sh","ace6t-gnome-session"),
                                        ("ace6t-touch-compat.py","ace6t-touch-compat"),
                                        ("ace6t-backlight-restore.py","ace6t-backlight-restore")]:
                        content=(scripts/name).read_text().rstrip()
                        if name=="ace6t-gnome.sh" and args.display_posted_start:
                            content=content.replace("unset WESTON_DISABLE_ATOMIC", "unset WESTON_DISABLE_ATOMIC\nexport ACE6T_WESTON_POSTED_START=1")
                            content=content.replace("${backlight_pid:-}", "${backlight_pid:-} ${driver_snapshot_pid:-}")
                            content=content.replace("python3 /usr/local/sbin/ace6t-touch-compat", "python3 /usr/local/sbin/ace6t-driver-snapshot --before\npython3 /usr/local/sbin/ace6t-driver-snapshot & driver_snapshot_pid=$!\npython3 /usr/local/sbin/ace6t-touch-compat")
                        lines += [f"cat > /usr/local/sbin/{target} <<'ACE6T_SCRIPT_EOF'",
                                  content,"ACE6T_SCRIPT_EOF",
                                  f"chmod 0755 /usr/local/sbin/{target}"]
                    if args.display_posted_start:
                        lines += ["cat > /usr/local/sbin/ace6t-driver-snapshot <<'ACE6T_SCRIPT_EOF'",
                                  (scripts/"ace6t-driver-snapshot.py").read_text().rstrip(),
                                  "ACE6T_SCRIPT_EOF", "chmod 0755 /usr/local/sbin/ace6t-driver-snapshot"]
                    lines += ["echo 'ACE6T_GNOME_LAUNCHERS_UPDATED: RAM only'"]
                    with tempfile.TemporaryDirectory(prefix="ace6t-gnome-") as tmp:
                        compat=Path(tmp)/"weston-format-compat.so"
                        subprocess.run(["clang","--target=aarch64-linux-gnu","-fuse-ld=lld",
                                        "-shared","-nostdlib","-fPIC","-O2","-Wall","-Wextra","-Werror",
                                        str(scripts/"weston-format-compat.c"),"-o",str(compat)],check=True)
                        binary=compat.read_bytes()
                        lines += ["mkdir -p /usr/local/lib",
                                  "base64 -d > /usr/local/lib/weston-format-compat.so <<'ACE6T_BINARY_EOF'",
                                  base64.b64encode(binary).decode(),"ACE6T_BINARY_EOF",
                                  f"echo '{hashlib.sha256(binary).hexdigest()}  /usr/local/lib/weston-format-compat.so' | sha256sum -c -",
                                  "chmod 0644 /usr/local/lib/weston-format-compat.so"]
                        if args.display_posted_start:
                            state=Path(tmp)/"ace6t-drm-state"
                            subprocess.run(["clang","--target=aarch64-linux-gnu","-fuse-ld=lld",
                                            "-Oz","-static","-nostdlib","-fno-builtin","-fno-stack-protector",
                                            "-fno-pie","-Wall","-Wextra","-Werror",
                                            "-Wl,--build-id=none,-e,_start,--no-dynamic-linker",
                                            str(scripts/"ace6t-drm-state.c"),"-o",str(state)],check=True)
                            binary=state.read_bytes()
                            lines += ["base64 -d > /usr/local/sbin/ace6t-drm-state <<'ACE6T_BINARY_EOF'",
                                      base64.b64encode(binary).decode(),"ACE6T_BINARY_EOF",
                                      f"echo '{hashlib.sha256(binary).hexdigest()}  /usr/local/sbin/ace6t-drm-state' | sha256sum -c -",
                                      "chmod 0755 /usr/local/sbin/ace6t-drm-state"]
                        script=Path(tmp)/"launchers.sh";script.write_text("\n".join(lines)+"\n")
                        data=script.read_bytes()
                        manifest["launcher_update"]=upload(fd,script,len(data),zlib.crc32(data),
                                                        hashlib.sha256(data).hexdigest(),"ubuntu-script")
                    capture("ubuntu-maintain")
                    for _ in range(20):
                        time.sleep(1);status=capture("ubuntu-status")
                        if b"child=0" in status:
                            if b"wait-status=0" not in status:
                                capture("ubuntu-log");raise OSError("RAM launcher update failed")
                            break
                    else:
                        raise TimeoutError("RAM launcher update timeout")
                display_command="display-swte" if args.display_swte else "display"
                commands=(display_command,"frame","touch","touch-enable","ubuntu-gnome") if args.rootfs else ("ubuntu-gnome",)
                for command in commands:
                    response=capture(command)
                    if any(word in response.lower() for word in (b"failed",b"need ",b"cannot ",b"already attempted")):
                        raise OSError(f"Device refused {command}; collect log")
                    if args.rebuilt_display and (command==display_command or
                                                 (args.software_input_fences and command=="frame")):
                        # Enable tracing through sysfs after the explicit driver load,
                        # before either the held frame or compositor submits a commit.
                        script_data=b"#!/bin/sh\nset -eu\nprintf 'Y\\n' > /sys/module/msm_drm/parameters/ace6t_trace\ncat /sys/module/msm_drm/parameters/ace6t_trace\necho ACE6T_REBUILT_DISPLAY_TRACE_ENABLED\n"
                        # CRTC debugfs is created on its first enabled frame.
                        # Set the feature mask afterward, before GNOME commits.
                        if args.software_input_fences and command=="frame":
                            script_data += software_input_fence_stage().encode()
                        with tempfile.TemporaryDirectory(prefix="ace6t-trace-") as tmp:
                            script=Path(tmp)/"trace.sh";script.write_bytes(script_data)
                            manifest["trace_enable"]=upload(fd,script,len(script_data),zlib.crc32(script_data),hashlib.sha256(script_data).hexdigest(),"ubuntu-script")
                        capture("ubuntu-maintain")
                        for _ in range(20):
                            time.sleep(1);status=capture("ubuntu-status")
                            if b"child=0" in status:
                                if b"wait-status=0" not in status:
                                    capture("ubuntu-log");raise OSError("Display trace enable failed")
                                break
                        else:
                            raise TimeoutError("Display trace enable timeout")
                # Allow the real compositor to initialise; then retain status/log.
                time.sleep(10)
        capture("ubuntu-status");capture("ubuntu-log");capture("drm");capture("input");capture("mounts");capture("log")
    except BaseException as error:
        manifest["error"]=str(error)
        raise
    finally:
        (args.out/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
        termios.tcsetattr(fd,termios.TCSANOW,old)
        fcntl.ioctl(fd,termios.TIOCNXCL);os.close(fd)


if __name__=="__main__":
    main()
