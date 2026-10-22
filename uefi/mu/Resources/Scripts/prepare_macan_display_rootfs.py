#!/usr/bin/env python3
"""Bundle the ABI-checked display module into a copy of the Ubuntu RAM archive."""
import argparse
import gzip
import hashlib
import json
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

from check_macan_kernel_abi import Elf

MODULE_PATH = "usr/local/share/ace6t/msm_drm-trace.ko"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(rootfs, workspace, abi_path, destination):
    if destination.exists() or destination.with_suffix(".manifest.json").exists():
        raise ValueError("Preserve existing archives; choose a new destination")
    record = json.loads(rootfs.with_suffix(".manifest.json").read_text())
    if record["desktop"] != "GNOME" or record["bytes"] != rootfs.stat().st_size or record["sha256"] != sha(rootfs):
        raise ValueError("Ubuntu archive differs from its manifest")
    abi = json.loads(abi_path.read_text())
    comparison = abi["display_comparison"]
    if abi["errors"] or not comparison["imports_identical"] or not comparison["exports_identical"]:
        raise ValueError("Stock/rebuilt display ABI agreement is required")
    module = workspace / "vendor/vendor/qcom/opensource/display-drivers/msm_drm.ko"
    if sha(module) != comparison["rebuilt_sha256"]:
        raise ValueError("Module differs from the checked build")
    selection = json.loads((workspace / "display-selection.json").read_text())
    if not selection["built"] or selection["module_sha256"] != sha(module):
        raise ValueError("Module differs from the recorded source selection")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(destination.parent).free < 5 * 1024**3:
        raise ValueError("Need 5 GiB free to retain a 4 GiB reserve")
    with tempfile.TemporaryDirectory(prefix="ace6t-display-rootfs-", dir=destination.parent) as tmp:
        stripped = Path(tmp) / "msm_drm.ko"
        shutil.copyfile(module, stripped)
        subprocess.run([workspace / "clang/bin/llvm-strip", "--strip-debug", stripped], check=True)
        original, replacement = Elf(module), Elf(stripped)
        if original.imports() != replacement.imports() or original.export_records() != replacement.export_records():
            raise ValueError("Stripping changed the display ABI")
        archive = Path(tmp) / "rootfs.tar.gz"
        with archive.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=3) as zipped:
            with tarfile.open(rootfs, "r|gz") as source, tarfile.open(fileobj=zipped, mode="w|", format=tarfile.GNU_FORMAT) as output:
                for item in source:
                    path = PurePosixPath(item.name)
                    if path.is_absolute() or ".." in path.parts or item.isdev() or item.isfifo():
                        raise ValueError(f"Unexpected archive path/type: {item.name}")
                    if str(path) == MODULE_PATH:
                        raise ValueError("Archive already contains the replacement")
                    output.addfile(item, source.extractfile(item) if item.isreg() else None)
                info = tarfile.TarInfo(MODULE_PATH)
                info.size = stripped.stat().st_size
                info.mode = 0o644
                with stripped.open("rb") as stream:
                    output.addfile(info, stream)
        if archive.stat().st_size > 512 * 1024**2:
            raise ValueError("Archive exceeds the USB RAM upload limit")
        record["base_archive_sha256"] = record["sha256"]
        record["sha256"] = sha(archive)
        record["bytes"] = archive.stat().st_size
        record["unpacked_regular_bytes"] += stripped.stat().st_size
        record["rebuilt_display"] = {"path": MODULE_PATH, "sha256": sha(stripped),
                                     "bytes": stripped.stat().st_size,
                                     "stock_sha256": comparison["stock_sha256"],
                                     "source_sha256": comparison["rebuilt_sha256"],
                                     "abi_report_sha256": sha(abi_path), "phone_loaded": False}
        record["rebuilt_display"]["software_input_fence_patch_sha256"] = selection.get("software_input_fence_patch_sha256")
        archive.rename(destination)
        destination.with_suffix(".manifest.json").write_text(json.dumps(record, indent=2) + "\n")
        destination.with_suffix(".sha256").write_text(f"{record['sha256']}  {destination.name}\n")
    print(f"Prepared {destination}: {record['bytes']} bytes; display {record['rebuilt_display']['sha256']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rootfs", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--abi", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.rootfs, args.workspace, args.abi, args.out)
