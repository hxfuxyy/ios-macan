"""Build a deterministic newc initramfs with a freestanding AArch64 PID 1."""
from pathlib import Path
import hashlib
import shutil
import stat
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DCP_SIZE = 8 * 1024 * 1024
DCP_SHA256 = "2129ba577fb54b08943b96135b40ff659655db2a979b537511f324f7f0f37ca4"


def make_archive(init: bytes, usb_modules: bytes | None = None,
                 display_modules: bytes | None = None, firmware: bytes | None = None,
                 touch_modules: bytes | None = None,
                 ubuntu_busybox: bytes | None = None) -> bytes:
    archive = bytearray()
    entries = [
        ("dev", stat.S_IFDIR | 0o755, b"", 0, 0),
        ("dev/console", stat.S_IFCHR | 0o600, b"", 5, 1),
        ("dev/kmsg", stat.S_IFCHR | 0o600, b"", 1, 11),
        ("init", stat.S_IFREG | 0o755, init, 0, 0),
    ]
    if usb_modules is not None:
        entries.append(("modules.usb.load", stat.S_IFREG | 0o444, usb_modules, 0, 0))
    if display_modules is not None:
        entries.extend([
            ("modules.display.load", stat.S_IFREG | 0o444, display_modules, 0, 0),
            ("lib", stat.S_IFDIR | 0o755, b"", 0, 0),
            ("lib/firmware", stat.S_IFDIR | 0o755, b"", 0, 0),
            ("lib/firmware/dcp.bin", stat.S_IFREG | 0o444, firmware, 0, 0),
        ])
    if touch_modules is not None:
        entries.append(("modules.touch.load", stat.S_IFREG | 0o444, touch_modules, 0, 0))
    if ubuntu_busybox is not None:
        entries.extend([
            ("bin", stat.S_IFDIR | 0o755, b"", 0, 0),
            ("bin/busybox", stat.S_IFREG | 0o755, ubuntu_busybox, 0, 0),
            ("ubuntu-ram.sh", stat.S_IFREG | 0o755,
             (ROOT / "Resources/LinuxRamTest/ubuntu-ram.sh").read_bytes(), 0, 0),
        ])
    entries.append(("TRAILER!!!", 0, b"", 0, 0))
    for inode, (name, mode, data, major, minor) in enumerate(entries, 1):
        filename = name.encode("ascii") + b"\0"
        fields = (inode, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(filename), 0)
        archive += b"070701" + b"".join(f"{v:08x}".encode("ascii") for v in fields)
        archive += filename
        archive += b"\0" * (-len(archive) % 4)
        archive += data
        archive += b"\0" * (-len(archive) % 4)
    archive += b"\0" * (-len(archive) % 512)
    return bytes(archive)


def prepare_ram_initrd(destination: Path, usb_test: bool = False,
                       display_firmware: Path | None = None,
                       ubuntu_busybox: Path | None = None,
                       display_trace: bool = False) -> Path:
    if display_trace and display_firmware is None:
        raise ValueError("Display tracing requires a rebuilt display module")
    helper = None
    if ubuntu_busybox is not None:
        if display_firmware is None:
            raise ValueError("Ubuntu RAM helper requires display diagnostics")
        helper = ubuntu_busybox.read_bytes()
        if len(helper) > 4 * 1024 * 1024 or helper[:6] != b"\x7fELF\x02\x01" or len(helper)<64:
            raise ValueError("Ubuntu helper must be a bounded static ARM64 ELF")
        if struct.unpack_from("<HH", helper, 16) != (2,183):
            raise ValueError("Ubuntu helper must be a static ARM64 executable")
        phoff = struct.unpack_from("<Q", helper, 32)[0]
        phsize, phcount = struct.unpack_from("<HH", helper, 54)
        if phsize<56 or phcount==0 or phoff+phsize*phcount>len(helper) or any(
            struct.unpack_from("<I", helper, phoff+i*phsize)[0] in (2,3) for i in range(phcount)
        ):
            raise ValueError("Ubuntu helper must have no dynamic loader or dependencies")
    firmware = None
    if display_firmware is not None:
        if not usb_test:
            raise ValueError("Display diagnostics require USB diagnostics")
        if display_firmware.stat().st_size != DCP_SIZE:
            raise ValueError("Display firmware differs from the saved 8 MiB Ace 6T dcp.bin")
        firmware = display_firmware.read_bytes()
        if hashlib.sha256(firmware).hexdigest() != DCP_SHA256:
            raise ValueError("Display firmware SHA256 differs from the verified stock capture")
    clang = shutil.which("clang")
    if clang is None or shutil.which("ld.lld") is None:
        raise ValueError("Linux RAM test requires clang and ld.lld")
    with tempfile.TemporaryDirectory(prefix="ace6t-init-") as temporary:
        executable = Path(temporary) / "init"
        subprocess.run([
            clang, "--target=aarch64-linux-gnu", "-Oz", "-static", "-nostdlib",
            "-fuse-ld=lld", "-fno-builtin", "-fno-stack-protector", "-fno-pie", "-no-pie",
            "-Wl,--build-id=none,-e,_start,--no-dynamic-linker",
            f"-DMACAN_LINUX_DISPLAY_TEST={int(display_firmware is not None)}",
            f"-DMACAN_UBUNTU_RAM={int(helper is not None)}",
            f"-DMACAN_DISPLAY_TRACE={int(display_trace)}",
            str(ROOT / "Resources/LinuxRamTest" / ("init_usb.c" if usb_test else "init.c")), "-o", str(executable),
        ], check=True, capture_output=True, text=True)
        init = executable.read_bytes()
        if init[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", init, 16) != (2, 183):
            raise ValueError("PID 1 must be an AArch64 ELF64 executable")
        phoff = struct.unpack_from("<Q", init, 32)[0]
        phsize, phcount = struct.unpack_from("<HH", init, 54)
        if any(struct.unpack_from("<I", init, phoff + i * phsize)[0] in (2, 3) for i in range(phcount)):
            raise ValueError("PID 1 must have no dynamic loader or dependencies")
        modules = display_modules = touch_modules = None
        if usb_test:
            modules = (ROOT / "Resources/LinuxRamTest/usb-modules.load").read_bytes()
            import re
            if not modules or len(modules) >= 8192 or any(
                not re.fullmatch(rb"[A-Za-z0-9_-][A-Za-z0-9_.-]*\.ko", line) or b".." in line
                for line in modules.splitlines()
            ):
                raise ValueError("USB module list must contain plain .ko filenames")
        if display_firmware is not None:
            display_modules = (ROOT / "Resources/LinuxRamTest/display-modules.load").read_bytes()
            if not display_modules or len(display_modules) >= 8192 or any(
                not re.fullmatch(rb"[A-Za-z0-9_-][A-Za-z0-9_.-]*\.ko", line) or b".." in line
                for line in display_modules.splitlines()
            ):
                raise ValueError("Display module list must contain plain .ko filenames")
            touch_modules = (ROOT / "Resources/LinuxRamTest/touch-modules.load").read_bytes()
            if not touch_modules or len(touch_modules) >= 8192 or any(
                not re.fullmatch(rb"[A-Za-z0-9_-][A-Za-z0-9_.-]*\.ko", line) or b".." in line
                for line in touch_modules.splitlines()
            ):
                raise ValueError("Touch module list must contain plain .ko filenames")
        archive = make_archive(init, modules, display_modules, firmware, touch_modules, helper)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(archive)
    return destination
