#!/usr/bin/env python3
"""Capture the Ace 6T RAM diagnostic console. --display explicitly loads DRM once."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import termios
import time

DEVICE = "/dev/serial/by-id/usb-Project_Silicium_Ace_6T_Linux_diagnostics_ace6t-linux-test-if00"
PROMPT = b"\r\nace6t> "


def exchange(fd, command, timeout=40):
    termios.tcflush(fd, termios.TCIFLUSH)
    os.write(fd, (command + "\n").encode("ascii"))
    data = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not select.select([fd], [], [], 0.25)[0]:
            continue
        try:
            part = os.read(fd, 65536)
        except BlockingIOError:
            continue
        if not part:
            raise OSError("USB console disconnected")
        data.extend(part)
        if len(data) > 3 * 1024 * 1024:
            raise OSError("Diagnostic response exceeds 3 MiB")
        if data.endswith(PROMPT):
            return bytes(data), True
    return bytes(data), False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default=DEVICE)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--display", action="store_true", help="Send the explicit display command after saving the boot snapshot; inspect DRM afterwards.")
    parser.add_argument("--frame", action="store_true", help="After --display, explicitly show a held 60 Hz color grid.")
    parser.add_argument("--frame-off", action="store_true", help="Explicitly disable the held frame and release its buffers.")
    parser.add_argument("--touch", action="store_true", help="After --display, explicitly load the stock SPI/HBP/S3910 drivers.")
    parser.add_argument("--touch-enable", action="store_true", help="Explicitly request stock touch IRQ and kernel LBP mode; needs a held frame.")
    parser.add_argument("--touch-events", action="store_true", help="Capture 20 seconds of touch EVDEV events after touch-enable.")
    args = parser.parse_args()
    if args.frame and (not args.display or args.frame_off):
        parser.error("--frame requires --display and cannot be combined with --frame-off")
    if args.touch and not args.display:
        parser.error("--touch requires --display")
    if args.frame_off and (args.touch or args.touch_enable or args.touch_events):
        parser.error("--frame-off cannot be combined with touch actions")
    if args.out.exists():
        parser.error("Output exists; choose a new --out to preserve captures")
    try:
        fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except PermissionError:
        parser.error("No device access; grant your user an ACL for this ttyACM device")
    old = termios.tcgetattr(fd)
    args.out.mkdir(parents=True)
    manifest = {"device": args.device, "display_requested": args.display,
                "frame_requested": args.frame, "frame_off_requested": args.frame_off,
                "touch_requested": args.touch, "touch_enable_requested": args.touch_enable,
                "touch_events_requested": args.touch_events, "files": {}}
    try:
        fcntl.ioctl(fd, termios.TIOCEXCL)
        raw = termios.tcgetattr(fd)
        raw[0] = raw[1] = raw[3] = 0
        raw[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        raw[4] = raw[5] = termios.B115200
        raw[6][termios.VMIN] = raw[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, raw)
        # Drain the device's initial greeting before sending a command.
        time.sleep(1)
        termios.tcflush(fd, termios.TCIFLUSH)
        commands = ["help", "usb", "version", "mem", "cpu", "modules", "input", "mounts", "drm", "bootlog"]
        if args.display:
            commands += ["display"]
            if args.frame:
                commands += ["frame"]
            commands += ["drm", "deferred", "modules", "usb"]
        if args.frame_off:
            commands += ["frame-off", "drm"]
        if args.touch:
            commands += ["touch", "touch-info", "deferred", "modules"]
        if args.touch_enable:
            commands += ["touch-enable", "touch-info"]
        if args.touch_events:
            commands += ["touch-events"]
        commands += ["log"]
        for index, command in enumerate(commands):
            data, complete = exchange(fd, command, timeout=60 if command in ("display", "touch", "touch-enable") else 40)
            name = f"{index:02d}-{command}.txt"
            (args.out / name).write_bytes(data)
            manifest["files"][name] = {"bytes": len(data), "prompt": complete, "sha256": hashlib.sha256(data).hexdigest()}
            print(f"{command}: {len(data)} bytes; prompt={complete}", flush=True)
            if not complete:
                raise OSError(f"{command}: no completion prompt; stopped before sending further commands")
            if command == "help":
                help_tokens = data.decode(errors="replace").split()
                if args.display and "display" not in help_tokens:
                    raise OSError("Connected image does not advertise the manual display command")
                if (args.frame or args.frame_off) and "frame-off" not in help_tokens:
                    raise OSError("Connected image does not advertise the manual frame command")
                for requested, name in ((args.touch, "touch"), (args.touch_enable, "touch-enable"), (args.touch_events, "touch-events")):
                    if requested and name not in help_tokens:
                        raise OSError(f"Connected image does not advertise {name}")
    finally:
        (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        termios.tcsetattr(fd, termios.TCSANOW, old)
        fcntl.ioctl(fd, termios.TIOCNXCL)
        os.close(fd)
    print(f"Saved {args.out.resolve()}")


if __name__ == "__main__":
    main()
