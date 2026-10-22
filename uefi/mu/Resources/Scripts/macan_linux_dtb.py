"""Stage an explicitly supplied SM8845 Linux DT; never use another phone's DT."""

import os
from pathlib import Path
import shutil
import struct
import subprocess


SOC_COMPATIBLES = {"qcom,sm8845", "qcom,molokai", "qcom,alor", "qcom,alorp"}


def prepare_linux_dtb(device: str, source: Path, package: Path) -> Path:
    if device != "macan":
        raise ValueError("--linux-dtb currently supports only -d macan.")
    data = source.read_bytes()
    if not 40 <= len(data) <= 2 * 1024 * 1024:
        raise ValueError("Expected one DTB between 40 bytes and 2 MiB.")
    magic, total = struct.unpack_from(">II", data)
    if magic != 0xD00DFEED or total != len(data):
        raise ValueError("Expected one complete DTB, not a boot image, DTBO or concatenated DTBs.")
    if not shutil.which("dtc") or not shutil.which("fdtget"):
        raise ValueError("Linux DT validation needs dtc and fdtget (device-tree-compiler / dtc package).")
    checked = subprocess.run(
        ["dtc", "-I", "dtb", "-O", "dtb", "-o", os.devnull, str(source)],
        capture_output=True, text=True,
    )
    if checked.returncode:
        raise ValueError(f"Invalid Linux DTB: {checked.stderr.strip()}")
    compatible = subprocess.run(
        ["fdtget", "-t", "s", str(source), "/", "compatible"],
        capture_output=True, text=True,
    )
    if compatible.returncode:
        raise ValueError(f"Cannot read DTB compatible: {compatible.stderr.strip()}")
    if not SOC_COMPATIBLES.intersection(compatible.stdout.split()):
        raise ValueError("DTB root compatible must identify SM8845 (molokai/alor/alorp).")
    # Checking the SoC does not establish board or kernel compatibility.
    destination = package / "FdtBlob" / "Linux.dtb"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    return destination
