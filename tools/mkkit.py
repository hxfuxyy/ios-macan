#!/usr/bin/env python3
import argparse
import io
import lzma
import os
import plistlib
import shutil
import tarfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAGE = ROOT / 'work' / 'sileo-root'
OBJ = ROOT / 'userland' / 'obj'
SILEO_DEB = ROOT / 'artifacts' / 'ace6t' / 'sileo' / 'org.coolstar.sileo_2.5.1_iphoneos-arm64.deb'
NOW = int(time.time())


def deb_member(path, prefix):
    data = path.read_bytes()
    if not data.startswith(b'!<arch>\n'):
        raise SystemExit(f'{path} is not a deb')
    at = 8
    while at + 60 <= len(data):
        header = data[at:at + 60]
        name = header[:16].decode().strip().rstrip('/')
        size = int(header[48:58].decode().strip())
        body = data[at + 60:at + 60 + size]
        if name.startswith(prefix):
            if name.endswith('.xz'):
                return lzma.decompress(body)
            if name.endswith('.zst'):
                import zstandard
                return zstandard.ZstdDecompressor().decompress(body, max_output_size=1 << 30)
            return body
        at += 60 + size + (size & 1)
    raise SystemExit(f'{prefix} not in {path}')


def add_dir(tar, name, mode=0o755):
    info = tarfile.TarInfo(name)
    info.type, info.mode, info.mtime = tarfile.DIRTYPE, mode, NOW
    tar.addfile(info)


def add_bytes(tar, name, data, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.mtime = len(data), mode, NOW
    tar.addfile(info, io.BytesIO(data))


def root_base(out):
    if not (STAGE / 'bootstrap').is_dir():
        raise SystemExit('mkkit: no staged Sileo root (work/sileo-root)')
    wifi_job = STAGE / 'LaunchDaemons' / 'com.redwood.wifi.plist'
    job = plistlib.loads(wifi_job.read_bytes())
    job['ProgramArguments'] = ['/redwood/bin/net_test', 'wait', '25', 'show', 'dhcp', 'en0', 'waitaddr', 'en0', '40',
                               'wait', '10', 'show', 'store', 'resolve', 'example.com', 'fetch', 'http://example.com/']
    wifi_job.write_bytes(plistlib.dumps(job))
    skel = STAGE / 'etc' / 'var.skel'
    if skel.exists():
        skel.unlink()

    def keep(info):
        rel = info.name.split('/', 1)[1] if '/' in info.name else ''
        if rel == 'fw' or rel.startswith('fw/'):
            return None
        info.uid = info.gid = 0
        info.uname = info.gname = ''
        return info

    with tarfile.open(out, 'w', format=tarfile.PAX_FORMAT) as tar:
        tar.add(STAGE, arcname='.', filter=keep)


def system_overlay(out):
    with tarfile.open(out, 'w', format=tarfile.PAX_FORMAT) as tar:
        for name in ('sbin', 'usr', 'usr/local', 'usr/local/redwood', 'Applications'):
            add_dir(tar, name)
        add_bytes(tar, 'sbin/launchd', (OBJ / 'stage0').read_bytes(), 0o755)
        mesa = OBJ / 'libOSMesa.8.dylib'
        if mesa.is_file():
            add_bytes(tar, 'usr/local/redwood/libOSMesa.8.dylib', mesa.read_bytes(), 0o755)
        if SILEO_DEB.is_file():
            with tarfile.open(fileobj=io.BytesIO(deb_member(SILEO_DEB, 'data.tar'))) as deb:
                prefix = './var/jb/Applications/'
                for member in deb:
                    if not member.name.startswith(prefix + 'Sileo.app'):
                        continue
                    name = 'Applications/' + member.name[len(prefix):]
                    mode = 0o4755 if name == 'Applications/Sileo.app/giveMeRoot' else member.mode
                    if member.isdir():
                        add_dir(tar, name, mode)
                    elif member.issym():
                        info = tarfile.TarInfo(name)
                        info.type, info.linkname, info.mode, info.mtime = tarfile.SYMTYPE, member.linkname, 0o755, NOW
                        tar.addfile(info)
                    elif member.isfile():
                        add_bytes(tar, name, deb.extractfile(member).read(), mode)


def main():
    parser = argparse.ArgumentParser(description='Collect what this machine built into work/kit.')
    parser.add_argument('--kit', type=Path, default=ROOT / 'work' / 'kit')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--uefi', type=Path)
    args = parser.parse_args()
    args.kit.mkdir(parents=True, exist_ok=True)
    root_base(args.kit / 'root-base.tar')
    system_overlay(args.kit / 'system-overlay.tar')
    if args.kernel:
        shutil.copyfile(args.kernel, args.kit / 'kernel')
    if args.uefi:
        shutil.copyfile(args.uefi, args.kit / 'uefi.img')
    tools = args.kit / 'tools'
    tools.mkdir(exist_ok=True)
    for name in ('unzbm', 'unzbm.exe'):
        for where in (ROOT / 'xnu-deps' / 'host' / 'bin' / name, ROOT / 'work' / 'tools' / name):
            if where.is_file():
                shutil.copyfile(where, tools / name)
                os.chmod(tools / name, 0o755)
                break
    print('mkkit:', ', '.join(sorted(p.name for p in args.kit.iterdir())))


if __name__ == '__main__':
    main()
