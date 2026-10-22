#!/usr/bin/env python3
"""Read bring-up data over rooted ADB; write only to the computer.

Requires Python 3.11+ and adb. Never flashes, reboots, or changes partitions.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile


PROPERTIES = (
    "ro.product.model", "ro.product.device", "ro.product.board",
    "ro.product.vendor.device", "ro.board.platform", "ro.soc.model",
    "ro.boot.hardware", "ro.boot.prjname", "ro.boot.project_name",
    "ro.boot.slot_suffix", "ro.boot.flash.locked", "ro.boot.verifiedbootstate",
    "ro.build.display.id", "ro.build.version.incremental",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="ADB device serial, required only with multiple phones.")
    parser.add_argument("--out", type=Path, default=Path("macan-data"))
    parser.add_argument("--backup-boot", action="store_true", help="Also read active-slot boot, init_boot, vendor_boot and dtbo to the computer.")
    args = parser.parse_args()
    if not shutil.which("adb"):
        parser.error("adb is not in PATH; install Android SDK Platform Tools.")
    devices = subprocess.run(["adb", "devices"], capture_output=True, text=True, check=True)
    available = [line.split()[0] for line in devices.stdout.splitlines()[1:]
                 if len(line.split()) == 2 and line.split()[1] == "device"]
    if args.serial:
        if args.serial not in available:
            parser.error("Selected phone is not authorized/connected in adb devices.")
        serial = args.serial
    elif len(available) == 1:
        serial = available[0]
    else:
        parser.error("Connect and authorize one phone, or choose it with --serial.")
    adb = ["adb", "-s", serial, "exec-out"]

    def read(command, root=False, timeout=30):
        return subprocess.run(adb + (["su", "-c", command] if root else ["sh", "-c", command]),
                              capture_output=True, timeout=timeout)

    uid = read("id -u", root=True, timeout=90)
    if uid.returncode or uid.stdout.strip() != b"0":
        parser.error("Grant the ADB shell root access in Magisk/KernelSU, then run again.")
    if args.out.exists() or args.out.with_suffix(".zip").exists():
        parser.error("Output already exists; use a new --out folder to preserve earlier captures.")
    args.out.mkdir(parents=True)
    manifest = {"properties": {}, "files": {}, "errors": {}}
    for prop in PROPERTIES:
        result = read(f"getprop {prop}")
        manifest["properties"][prop] = result.stdout.decode(errors="replace").strip()

    captures = {
        "running.dtb": "cat /sys/firmware/fdt",
        "iomem.txt": "cat /proc/iomem",
        "meminfo.txt": "cat /proc/meminfo",
        "cmdline.txt": "cat /proc/cmdline",
        "bootconfig.txt": "cat /proc/bootconfig",
        "kernel.txt": "uname -a",
        "config.gz": "cat /proc/config.gz",
        "partitions.txt": "ls -l /dev/block/by-name",
        "dmesg.txt": "dmesg",
        "pstore.txt": 'for f in /sys/fs/pstore/*; do if [ -f "$f" ]; then cat "$f"; fi; done',
    }
    for filename, command in captures.items():
        result = read(command, root=True)
        data = result.stdout
        if filename == "running.dtb":
            if len(data) < 40 or data[:4] != b"\xd0\x0d\xfe\xed" or struct.unpack_from(">I", data, 4)[0] != len(data):
                manifest["errors"][filename] = "No complete live FDT obtained; need stock vendor_boot + dtbo instead."
                continue
        if result.returncode:
            manifest["errors"][filename] = result.stderr.decode(errors="replace").strip() or f"exit {result.returncode}"
            continue
        (args.out / filename).write_bytes(data)
        manifest["files"][filename] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    if args.backup_boot:
        slot = manifest["properties"]["ro.boot.slot_suffix"]
        if slot not in ("", "_a", "_b"):
            raise ValueError("Unrecognized slot suffix; refusing to construct block paths.")
        for partition in ("boot", "init_boot", "vendor_boot", "dtbo"):
            block = f"/dev/block/by-name/{partition}{slot}"
            filename = f"{partition}{slot}.img"
            if read(f"test -r {block}", root=True).returncode:
                manifest["errors"][filename] = "Active-slot partition not readable/present."
                continue
            path = args.out / filename
            with path.open("wb") as stream:
                result = subprocess.run(adb + ["su", "-c", f"cat {block}"],
                                        stdout=stream, stderr=subprocess.PIPE, timeout=180)
            if result.returncode or not path.stat().st_size:
                path.unlink()
                manifest["errors"][filename] = "Partition read failed."
                continue
            manifest["files"][filename] = {"size": path.stat().st_size,
                                           "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}

    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    archive = args.out.with_suffix(".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(args.out.iterdir()):
            bundle.write(path, path.name)
    print(f"Saved {archive.resolve()}")
    print("Model:", manifest["properties"]["ro.product.model"])
    if manifest["errors"]:
        print("Some optional captures are unavailable; see manifest.json.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Collection failed: {error}", file=sys.stderr)
        sys.exit(1)
