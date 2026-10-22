#!/usr/bin/env python3
"""Read stock display/input setup via root ADB; write captures only on the PC."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


CAPTURES = {
    "cmdline.txt": "cat /proc/cmdline",
    "bootconfig.txt": "cat /proc/bootconfig",
    "input-devices.txt": "cat /proc/bus/input/devices",
    "input-sysfs.txt": '''for d in /sys/class/input/event*; do
        [ -e "$d" ] || continue
        echo "$d"; cat "$d/dev" "$d/device/name" "$d/device/phys" 2>/dev/null
        readlink -f "$d/device"
        done''',
    "drm-sysfs.txt": '''for d in /sys/class/drm/*; do
        [ -e "$d" ] || continue
        echo "$d"; readlink -f "$d/device/driver"
        for f in dev status enabled modes; do
            [ ! -r "$d/$f" ] || { echo "$f"; cat "$d/$f"; }
        done
        done''',
    "display-devices.txt": "ls -l /dev/dri /dev/input /dev/graphics /dev/*hbp* /dev/*tcm* 2>/dev/null",
    "backlight.txt": '''for d in /sys/class/backlight/* /sys/class/leds/lcd-backlight; do
        [ -d "$d" ] || continue
        echo "$d"
        for f in brightness max_brightness actual_brightness; do
            [ ! -r "$d/$f" ] || { echo "$f"; cat "$d/$f"; }
        done
        done''',
    "modules.txt": "cat /proc/modules",
    "processes.txt": "ps -A -o USER,PID,NAME,ARGS",
    "dmesg.txt": "dmesg",
    "firmware-inventory.txt": '''for d in /vendor/firmware /vendor/firmware_mnt/image /odm/firmware; do
        [ ! -d "$d" ] || find "$d" -type f
        done''',
    "firmware-links.txt": "ls -ld /vendor/firmware /vendor/firmware_mnt /vendor/firmware_mnt/image /odm/firmware 2>/dev/null",
    "vendor-init.tar": "tar -C /vendor/etc -cf - init",
    "ueventd.txt": '''for f in /vendor/etc/ueventd.rc /odm/etc/ueventd.rc; do
        [ ! -r "$f" ] || { echo "$f"; cat "$f"; }
        done''',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not shutil.which("adb"):
        parser.error("adb is not in PATH")
    result = subprocess.run(["adb", "devices"], check=True, capture_output=True, text=True)
    devices = [r.split()[0] for r in result.stdout.splitlines()[1:]
               if len(r.split()) == 2 and r.split()[1] == "device"]
    serial = args.serial or (devices[0] if len(devices) == 1 else None)
    if serial not in devices:
        parser.error("Connect one authorized Android phone or choose --serial")
    adb = ["adb", "-s", serial, "exec-out", "su", "-c"]
    result = subprocess.run(adb + ["id -u"], capture_output=True, timeout=90)
    if result.returncode or result.stdout.strip() != b"0":
        parser.error("Grant ADB shell root access in Magisk/KernelSU")
    if args.out.exists():
        parser.error("Output exists; choose a new --out to preserve captures")
    args.out.mkdir(parents=True)
    manifest = {"serial": serial, "files": {}, "errors": {}}
    for name, command in CAPTURES.items():
        try:
            result = subprocess.run(adb + [command], capture_output=True, timeout=60)
        except subprocess.TimeoutExpired:
            manifest["errors"][name] = "ADB read timed out"
            continue
        # Preserve useful optional output even if some sysfs/device paths are absent.
        if result.stdout:
            (args.out / name).write_bytes(result.stdout)
            manifest["files"][name] = {
                "bytes": len(result.stdout),
                "sha256": hashlib.sha256(result.stdout).hexdigest(),
            }
        if result.returncode:
            manifest["errors"][name] = result.stderr.decode(errors="replace").strip() or str(result.returncode)
        print(f"{name}: {len(result.stdout)} bytes", flush=True)
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Saved {args.out.resolve()}")


if __name__ == "__main__":
    main()
