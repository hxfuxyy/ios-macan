#!/usr/bin/env python3
"""Read a working Ace 6T display over root ADB; restore debugfs read windows."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import struct
import subprocess


def register_window(base, name, offset, count):
    """Select a driver-validated read range; never write the *_reg endpoint."""
    if name not in ("sde", "dsi0_ctrl", "scc_3") or offset < 0 or not 0 < count <= 0x350:
        raise ValueError("Unexpected display read window")
    selector = shlex.quote(str(Path(base) / (name + "_off")))
    registers = shlex.quote(str(Path(base) / (name + "_reg")))
    return f"""set -eu
selector={selector}
registers={registers}
old_selection=$(cat "$selector")
old_offset=${{old_selection%% *}}
old_count=${{old_selection#* }}
case "$old_offset:$old_count" in *[!0-9a-fA-FxX:]*|'') exit 1;; esac
[ -n "$old_offset" ] && [ -n "$old_count" ]
# The driver's %6x parser rejects its own zero-padded readback. Restore the
# same numeric range in compact hex, without writing any register value.
restore() {{ printf '%x %x\\n' "$((old_offset))" "$((0x$old_count))" > "$selector"; }}
trap restore EXIT
trap 'exit 1' HUP INT TERM
printf '{offset:x} {count:x}\\n' > "$selector"
cat "$registers"
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("Choose a new output to preserve earlier evidence")
    devices = subprocess.run(["adb", "devices"], capture_output=True, text=True,
                             check=True, timeout=15)
    connected = [row.split()[0] for row in devices.stdout.splitlines()[1:]
                 if len(row.split()) == 2 and row.split()[1] == "device"]
    serial = args.serial or (connected[0] if len(connected) == 1 else None)
    if serial not in connected:
        parser.error("Connect the authorized Android phone")
    adb = ["adb", "-s", serial, "exec-out", "su", "-c"]

    def read(command):
        # exec-out does not reliably carry the remote command's exit status.
        wrapped = ("/system/bin/sh -c " + shlex.quote(command) +
                   "; result=$?; printf '\\nACE6T_ADB_STATUS=%d\\n' \"$result\"")
        result = subprocess.run(adb + [wrapped], capture_output=True, timeout=30)
        data, marker, status = result.stdout.rpartition(b"\nACE6T_ADB_STATUS=")
        if result.returncode or not marker or status.strip() != b"0":
            raise RuntimeError(result.stderr.decode(errors="replace").strip() or
                               data.decode(errors="replace")[-1000:] or
                               "ADB command failed or omitted its exit status")
        return data

    if read("id -u").strip() != b"0":
        parser.error("Root ADB access required")
    if read("getprop ro.product.model").strip() != b"PLR110":
        parser.error("This register map is verified only for PLR110")
    # Verify the actual running DT before selecting hardware block addresses.
    dt = "/sys/firmware/devicetree/base/soc/qcom,mdss_mdp@9800000"
    ctl = read(f"cat {dt}/qcom,sde-ctl-off")
    intf = read(f"cat {dt}/qcom,sde-intf-off")
    if len(ctl) != 24 or len(intf) != 16:
        parser.error("Unexpected display DT layout")
    if struct.unpack(">6I", ctl)[0] != 0x20000 or struct.unpack(">4I", intf)[1] != 0x18f000:
        parser.error("Display DT offsets differ from the Linux capture")
    base = "/sys/kernel/debug/dri/0/debug"
    captures = {
        "identity.txt": "getprop ro.product.model; uname -a; cat /proc/uptime",
        "cesta.txt": "cat /sys/kernel/debug/sde_cesta_0/status",
        "irq-before.txt": f"cat {base}/core_irq",
        "encoder.txt": "cat /sys/kernel/debug/dri/0/encoder69/status",
        "fences.txt": f"cat {base}/hw_fence_status /sys/kernel/debug/dri/0/crtc203/hwfence_features_mask",
        "ctl0.txt": register_window(base, "sde", 0x20000, 0x350),
        "intf1.txt": register_window(base, "sde", 0x18f000, 0x300),
        "dsi0.txt": register_window(base, "dsi0_ctrl", 0, 0x200),
        "scc3.txt": register_window(base, "scc_3", 0, 0x20),
        "drm-state.txt": "cat /sys/kernel/debug/dri/0/state",
        "clock-summary.txt": "cat /sys/kernel/debug/clk/clk_summary",
        "irq-after.txt": f"cat {base}/core_irq",
    }
    args.out.mkdir(parents=True)
    manifest = {"serial": serial, "files": {}, "errors": {},
                "model": "PLR110", "register_values_written": False,
                "read_window_restoration": "EXIT/HUP/INT/TERM traps",
                "debugfs": "virtual mount, if needed; removed on exit",
                "ctl_dt_hex": ctl.hex(), "intf_dt_hex": intf.hex()}
    try:
        # One root process keeps the virtual debugfs mount in one namespace.
        # It has no block device and creates no files in Android userdata.
        batch = ["set -eu", "created=0",
                 "if ! awk '$2==\"/sys/kernel/debug\" && $3==\"debugfs\" {found=1} END {exit !found}' /proc/mounts; then",
                 "mount -t debugfs -o nosuid,nodev,noexec debugfs /sys/kernel/debug",
                 "created=1", "fi",
                 'cleanup() { if [ "$created" = 1 ]; then umount /sys/kernel/debug; fi; }',
                 "trap cleanup EXIT", "trap 'exit 1' HUP INT TERM"]
        for name, command in captures.items():
            batch += [f"printf '\\nACE6T_CAPTURE_BEGIN={name}\\n'",
                      "if /system/bin/sh -c " + shlex.quote(command) +
                      "; then status=0; else status=$?; fi",
                      f"printf '\\nACE6T_CAPTURE_END={name}:%d\\n' \"$status\""]
        output = read("\n".join(batch))
        for name in captures:
            begin = f"\nACE6T_CAPTURE_BEGIN={name}\n".encode()
            end = f"\nACE6T_CAPTURE_END={name}:".encode()
            _, start, rest = output.partition(begin)
            data, finish, rest = rest.partition(end)
            status = rest.split(b"\n", 1)[0]
            if start and finish and status == b"0":
                (args.out / name).write_bytes(data)
                manifest["files"][name] = {"bytes": len(data),
                                           "sha256": hashlib.sha256(data).hexdigest()}
                print(f"{name}: {len(data)} bytes", flush=True)
            else:
                manifest["errors"][name] = data.decode(errors="replace")[-1000:] or "Capture missing"
                print(f"{name}: unavailable", flush=True)
    finally:
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
