"""Make a DT handoff probe from a captured SM8845 DT, removing old boot state.

This does not turn an Android DT into a mainline DT or verify its memory map.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from Resources.Scripts.macan_linux_dtb import prepare_linux_dtb


STALE_CHOSEN = ("linux,initrd-start", "linux,initrd-end", "bootargs", "kaslr-seed", "rng-seed")


def make_efi_probe_dtb(source: Path, destination: Path) -> Path:
    if destination.exists():
        raise ValueError("Refusing to overwrite an existing candidate DTB.")
    if not shutil.which("fdtput"):
        raise ValueError("Needs fdtput from device-tree-compiler / dtc.")
    # Use the same real DT parser and SoC checks as the build, without staging
    # an unmodified live DT into the actual platform directory.
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        candidate = prepare_linux_dtb("macan", source, root)
        properties = subprocess.run(["fdtget", "-p", str(candidate), "/chosen"],
                                    capture_output=True, text=True, check=True).stdout.splitlines()
        for property_name in STALE_CHOSEN:
            if property_name in properties:
                subprocess.run(["fdtput", "-d", str(candidate), "/chosen", property_name], check=True)
        # Reparse the resulting tree; keep all hardware nodes/carveouts intact.
        validated = prepare_linux_dtb("macan", candidate, root / "validated")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as output:
            output.write(validated.read_bytes())
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    make_efi_probe_dtb(args.source, args.destination)
    print(args.destination)


if __name__ == "__main__":
    main()
