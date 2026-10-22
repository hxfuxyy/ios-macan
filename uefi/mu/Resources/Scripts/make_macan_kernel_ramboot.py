#!/usr/bin/env python3
"""Package the verified custom kernel and traced display in a RAM-only boot image.

Direct Android header-v4 boot is an untested transport candidate. This does
not replace the UEFI loader, embed Ubuntu, or issue any phone commands.
"""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import stat
import struct
import subprocess
import tempfile
from pathlib import Path, PurePosixPath

from build_macan_kernel import ROOT, RESERVE
from check_macan_kernel_abi import Elf
from make_macan_ram_initrd import prepare_ram_initrd

CMDLINE = "rdinit=/init init=/ace6t-no-disk-init root=/dev/ram0 ro panic=0 kvm-arm.mode=nvhe"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def newc(entries):
    data = bytearray()
    seen = set()
    for inode, (name, mode, payload) in enumerate([*entries, ("TRAILER!!!", 0, b"")], 1):
        path = PurePosixPath(name)
        if not name or "\0" in name or path.is_absolute() or ".." in path.parts or str(path) != name or name in seen:
            raise ValueError("Invalid or duplicate initramfs pathname")
        seen.add(name)
        filename = name.encode("ascii") + b"\0"
        fields = (inode, mode, 0, 0, 1, 0, len(payload), 0, 0, 0, 0, len(filename), 0)
        data += b"070701" + b"".join(f"{value:08x}".encode() for value in fields)
        data += filename
        data += b"\0" * (-len(data) % 4)
        data += payload
        data += b"\0" * (-len(data) % 4)
    data += b"\0" * (-len(data) % 512)
    return bytes(data)


def validate_image(data, kernel, ramdisk):
    if len(data) < 4096 or data[:8] != b"ANDROID!":
        raise ValueError("Missing Android boot header")
    ks, rs, os_version, hs = struct.unpack_from("<IIII", data, 8)
    version = struct.unpack_from("<I", data, 40)[0]
    signature = struct.unpack_from("<I", data, 1580)[0]
    # The bundled AOSP mkbootimg writes a 4 KiB zero placeholder when no
    # GKI signing key is supplied. Verify it instead of implying a signature.
    if (ks, rs, os_version, hs, version, signature) != (len(kernel), len(ramdisk), 0, 1584, 4, 4096):
        raise ValueError("Unexpected boot header fields")
    if data[44:1580].split(b"\0")[0] != CMDLINE.encode():
        raise ValueError("Unexpected boot command line")
    ramdisk_start = 4096 + ((ks + 4095) // 4096) * 4096
    if data[4096:4096 + ks] != kernel or data[ramdisk_start:ramdisk_start + rs] != ramdisk:
        raise ValueError("Boot payload differs from verified inputs")
    signature_start = ramdisk_start + ((rs + 4095) // 4096) * 4096
    if len(data) != signature_start + signature or any(data[signature_start:]):
        raise ValueError("Unexpected trailing boot data")
    if len(kernel) < 64 or kernel[56:60] != b"ARM\x64":
        raise ValueError("Expected native ARM64 Image")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--abi-report", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    workspace, destination = args.workspace.resolve(), args.out.resolve()
    manifest_path = destination.with_suffix(".json")
    if destination.exists() or manifest_path.exists():
        raise ValueError("Refusing to overwrite a boot candidate or manifest")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(destination.parent).free < RESERVE:
        raise RuntimeError("Fewer than 4 GiB free")
    build = json.loads((workspace / "kernel-build.json").read_text())
    kernel_path = workspace / "out/arch/arm64/boot/Image"
    if sha(kernel_path) != build["files"]["arch/arm64/boot/Image"]["sha256"]:
        raise ValueError("Kernel differs from completed build")
    abi = json.loads(args.abi_report.read_text())
    if not abi["crc_check_passed"] or abi["errors"] or not abi["display_comparison"]:
        raise ValueError("Rebuilt display ABI check required")
    if sha(workspace / "out/Module.symvers") != abi["kernel_symvers_sha256"]:
        raise ValueError("ABI report belongs to a different kernel")
    source_module = workspace / "vendor/vendor/qcom/opensource/display-drivers/msm_drm.ko"
    original = Elf(source_module)
    if hashlib.sha256(original.data).hexdigest() != abi["display_comparison"]["rebuilt_sha256"]:
        raise ValueError("Display differs from checked module")
    with tempfile.TemporaryDirectory(prefix="ace6t-kernel-ram-", dir=destination.parent) as temp:
        temporary = Path(temp)
        stripped = temporary / "msm_drm.ko"
        shutil.copyfile(source_module, stripped)
        subprocess.run([workspace / "clang/bin/llvm-strip", "--strip-debug", stripped], check=True)
        replacement = Elf(stripped)
        if original.imports() != replacement.imports() or original.export_records() != replacement.export_records():
            raise ValueError("Debug stripping changed the module interface")
        entries = [(name, stat.S_IFDIR | 0o755, b"") for name in ("lib", "lib/modules")]
        packed_modules = {}
        for name, record in sorted(abi["modules"].items()):
            if name == "msm_drm.ko":
                source, payload = source_module, replacement.data
            else:
                source = ROOT / "artifacts/ace6t/vendor-ramdisk-extracted/lib/modules" / name
                if not source.is_file():
                    source = ROOT / "artifacts/ace6t/android-after-ram-test" / name
                if source.is_symlink() or sha(source) != record["sha256"]:
                    raise ValueError(f"Stock module changed: {name}")
                payload = source.read_bytes()
            entries.append((f"lib/modules/{name}", stat.S_IFREG | 0o444, payload))
            packed_modules[name] = {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
        initrd = temporary / "init.cpio"
        old_path = os.environ.get("PATH", "")
        os.environ["PATH"] = str(workspace / "clang/bin") + ":" + old_path
        try:
            prepare_ram_initrd(initrd, usb_test=True, display_trace=True,
                               display_firmware=ROOT / "artifacts/ace6t/android-graphics-capture/dcp.bin",
                               ubuntu_busybox=ROOT / "artifacts/ubuntu/busybox-arm64")
        finally:
            os.environ["PATH"] = old_path
        # Linux accepts concatenated, padded newc archives. Put our modules
        # after the bootstrap archive and after any bootloader vendor ramdisk.
        archive = initrd.read_bytes() + newc(entries)
        ramdisk_path = temporary / "initramfs.cpio.gz"
        ramdisk = gzip.compress(archive, compresslevel=9, mtime=0)
        ramdisk_path.write_bytes(ramdisk)
        image = temporary / "boot.img"
        subprocess.run(["python3", ROOT / "Resources/Scripts/mkbootimg.py",
                        "--header_version", "4", "--kernel", kernel_path,
                        "--ramdisk", ramdisk_path, "--cmdline", CMDLINE,
                        "--output", image], check=True)
        kernel = kernel_path.read_bytes()
        data = image.read_bytes()
        validate_image(data, kernel, ramdisk)
        if gzip.decompress(ramdisk) != archive:
            raise ValueError("Ramdisk round-trip failed")
        with destination.open("xb") as file:
            file.write(data)
    manifest_path.write_text(json.dumps({
        "transport": "direct Android boot header v4; bootloader supplies stock DT/vendor ramdisk",
        "phone_booted": False, "physical_gnome_verified": False,
        "image_bytes": len(data), "image_sha256": hashlib.sha256(data).hexdigest(),
        "kernel_sha256": hashlib.sha256(kernel).hexdigest(),
        "initramfs_bytes": len(archive), "ramdisk_bytes": len(ramdisk),
        "ramdisk_sha256": hashlib.sha256(ramdisk).hexdigest(),
        "cmdline": CMDLINE, "modules": packed_modules,
        "abi_report_sha256": sha(args.abi_report),
        "ubuntu_embedded": False,
        "mount_policy": "PID 1 mounts virtual filesystems; Ubuntu is uploaded to tmpfs; no Android block mounts",
        "limitations": "Bootloader handoff, module loading, CFI, signatures and physical display still need device verification",
    }, indent=2) + "\n")
    print(f"RAM boot candidate: {destination} ({len(data)} bytes); not yet booted on phone")


if __name__ == "__main__":
    main()
