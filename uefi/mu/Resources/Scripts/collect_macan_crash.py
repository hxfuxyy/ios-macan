#!/usr/bin/env python3
"""Read preserved crash records via rooted Android ADB; save only on the PC."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', default='3B661U00C1500000')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists():
        parser.error('Choose a new --out to preserve earlier captures')
    adb = ['adb', '-s', args.serial, 'exec-out', 'su', '-c']
    result = subprocess.run(adb + ['id -u'], capture_output=True, timeout=30)
    if result.returncode or result.stdout.strip() != b'0':
        parser.error('Connect the authorized Android phone and grant root ADB access')
    args.out.mkdir(parents=True)
    captures = {
        'pstore.tar': 'tar -C /sys/fs/pstore -cf - .',
        'pstore-list.txt': 'ls -l /sys/fs/pstore',
        'last-kmsg.txt': 'cat /proc/last_kmsg',
        'boot-reasons.txt': 'for p in ro.boot.bootreason sys.boot.reason sys.boot.reason.last persist.sys.boot.reason; do echo "$p"; getprop "$p"; done',
        'android-dmesg.txt': 'dmesg',
        'kernel.txt': 'uname -a',
    }
    manifest = {'serial': args.serial, 'files': {}, 'errors': {}}
    try:
        for name, command in captures.items():
            try:
                result = subprocess.run(adb + [command], capture_output=True, timeout=30)
                if result.stdout:
                    (args.out / name).write_bytes(result.stdout)
                    manifest['files'][name] = {
                        'bytes': len(result.stdout),
                        'sha256': hashlib.sha256(result.stdout).hexdigest(),
                    }
                if result.returncode:
                    manifest['errors'][name] = result.stderr.decode(errors='replace').strip()
                print(f'{name}: {len(result.stdout)} bytes', flush=True)
            except subprocess.TimeoutExpired:
                manifest['errors'][name] = 'ADB read timed out'
    finally:
        (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
