#!/usr/bin/env python3

from pathlib import Path, PurePosixPath
import argparse, hashlib, io, json, os, re, shutil, subprocess, tarfile

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'artifacts/ace6t/sileo'
DEBS = ASSETS / 'debs'
ELLEKIT = ('ellekit_0.4.5_iphoneos-arm64.deb', 'https://ellekit.space/pool/ellekit_0.4.5_iphoneos-arm64.deb')
PREFERENCELOADER = ('preferenceloader_2.2.6-1_iphoneos-arm64.deb',
                    'https://apt.procurs.us/pool/main/iphoneos-arm64-rootless/1900/'
                    'preferenceloader_2.2.6-1_iphoneos-arm64.deb')
DROP_PACKAGES = ('libkrw0', 'libdimentio0', 'shshd')
OLD_TWEAKS = 'var/jb/Library/MobileSubstrate/DynamicLibraries/'
NEW_TWEAKS = 'var/jb/usr/lib/TweakInject/'

UIKITTOOLS_POSTINST = '''#!/bin/sh

set -e

case "$1" in
    (triggered)
        for app in /var/jb/Applications/*.app; do
            [ -d "$app" ] || continue
            [ "${app##*/}" = Sileo.app ] && continue
            uicache -p "$app" || true
        done
        ;;
    (*)
        exit 0
        ;;
esac
'''

PREP = '''#!/var/jb/bin/sh
/var/jb/usr/libexec/firmware
/var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd
for package in debianutils apt dash bash zsh vi; do
\t[ -x "/var/jb/Library/dpkg/info/$package.postinst" ] &&
\t\t"/var/jb/Library/dpkg/info/$package.postinst" configure 99999 >/dev/null 2>&1
done
exit 0
'''

RC = '''/redwood/bin/bootstrap-copy
/redwood/bin/persona-init
/var/jb/bin/sh /var/jb/prep.sh
exec /sbin/launchd.ios
'''

INJECT = [
    '/redwood/lib/libredwood-tweaks.dylib Preferences',
    '/redwood/lib/libredwood-app.dylib Sileo NewTerm',
]

def fetch(name, url):
    path = DEBS / name
    if not path.exists():
        DEBS.mkdir(parents=True, exist_ok=True)
        subprocess.run(['curl', '-sL', '--fail', '-o', str(path), url], check=True)
    return path

def deb_member(deb, prefix):
    names = subprocess.run(['ar', 't', str(deb)], capture_output=True, text=True, check=True).stdout.split()
    member = next(n for n in names if n.startswith(prefix))
    data = subprocess.check_output(['ar', 'p', str(deb), member])
    if member.endswith('.zst'):
        return subprocess.check_output(['zstd', '-dc'], input=data)
    if member.endswith('.xz'):
        return subprocess.check_output(['xz', '-dc'], input=data)
    return data

def deb_control(deb):
    with tarfile.open(fileobj=io.BytesIO(deb_member(deb, 'control.tar')), mode='r:') as tar:
        member = next(m for m in tar if m.name.lstrip('./') == 'control')
        return tar.extractfile(member).read().decode().strip('\n')

def ingest(entries, data, remap_tweaks=False):
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as tar:
        for member in tar:
            rel = PurePosixPath(member.name.removeprefix('./'))
            if rel.parts[:2] != ('var', 'jb'):
                continue
            if any(x in member.name for x in '\t\n') or '..' in rel.parts:
                raise ValueError('unsafe package path ' + member.name)
            name = str(rel)
            if remap_tweaks and (name + '/').startswith(OLD_TWEAKS) and not member.isdir():
                name = NEW_TWEAKS + name[len(OLD_TWEAKS):]
            if member.isdir():
                if remap_tweaks and (name + '/') == OLD_TWEAKS:
                    continue
                if entries.get(name, ('d',))[0] == 'l':
                    continue
                entries[name] = ('d', member.mode, member.uid, member.gid, None)
            elif member.issym():
                entries[name] = ('l', member.mode, member.uid, member.gid, member.linkname)
            elif member.isfile() or member.islnk():
                entries[name] = ('f', member.mode, member.uid, member.gid, tar.extractfile(member).read())

def stanza_name(stanza):
    m = re.search(r'(?m)^Package: (.+)$', stanza)
    return m.group(1).strip() if m else None

def edit_status(text, added_controls):
    stanzas = [s.strip('\n') for s in text.split('\n\n') if s.strip()]
    stanzas = [s for s in stanzas if stanza_name(s) not in DROP_PACKAGES]
    have = {stanza_name(s) for s in stanzas}
    for control in added_controls:
        if stanza_name(control) not in have:
            stanzas.append(control.strip('\n') + '\nStatus: install ok installed')
    out = []
    for stanza in stanzas:
        lines = []
        for line in stanza.split('\n'):
            key = line.split(':', 1)[0]
            if key in ('Depends', 'Pre-Depends'):
                groups = [g for g in line[len(key) + 2:].split(', ')
                          if not re.match(r'^\s*firmware\s*(\(|$)', g.split(' | ')[0].strip())]
                if not groups:
                    continue
                line = key + ': ' + ', '.join(groups)
            lines.append(line)
        out.append('\n'.join(lines))
    return '\n\n'.join(out) + '\n\n'

def main():
    parser = argparse.ArgumentParser(description='')
    parser.add_argument('--stage', type=Path, default=ROOT / 'work/sileo-root')
    parser.add_argument('--pack', default=str(ASSETS / 'root-sileo-wifi.packfs'),
                        help='packfs to write afterwards (empty string: do not pack)')
    args = parser.parse_args()
    stage = args.stage
    if not (stage / 'bootstrap').is_dir():
        raise SystemExit('no staged Sileo root at %s (run prepare_sileo.py first)' % stage)

    for metadata, name in (('bootstrap-provenance.json', 'bootstrap-iphoneos-arm64.tar.zst'),
                           ('package-provenance.json', 'org.coolstar.sileo_2.5.1_iphoneos-arm64.deb')):
        data = (ASSETS / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != json.loads((ASSETS / metadata).read_text())['sha256']:
            raise SystemExit('checksum mismatch: ' + name)
    sileo = ASSETS / 'org.coolstar.sileo_2.5.1_iphoneos-arm64.deb'
    ellekit = fetch(*ELLEKIT)
    preferenceloader = fetch(*PREFERENCELOADER)

    entries = {}
    ingest(entries, subprocess.check_output(['zstd', '-dc', str(ASSETS / 'bootstrap-iphoneos-arm64.tar.zst')]))
    ingest(entries, deb_member(sileo, 'data.tar'))
    ingest(entries, deb_member(ellekit, 'data.tar'))
    ingest(entries, deb_member(preferenceloader, 'data.tar'), remap_tweaks=True)
    entries['var/jb/usr/lib/TweakInject'] = ('d', 0o755, 0, 0, None)
    entries[OLD_TWEAKS.rstrip('/')] = ('l', 0o755, 0, 0, '/var/jb/usr/lib/TweakInject')

    for name in list(entries):
        for package in DROP_PACKAGES:
            if name.startswith('var/jb/Library/dpkg/info/%s.' % package):
                del entries[name]
    status = 'var/jb/Library/dpkg/status'
    kind, mode, uid, gid, body = entries[status]
    controls = [deb_control(sileo), deb_control(ellekit), deb_control(preferenceloader)]
    entries[status] = (kind, mode, uid, gid, edit_status(body.decode(), controls).encode())
    postinst = 'var/jb/Library/dpkg/info/uikittools.postinst'
    kind, mode, uid, gid, _ = entries[postinst]
    entries[postinst] = (kind, mode, uid, gid, UIKITTOOLS_POSTINST.encode())
    entries['var/jb/prep.sh'] = ('f', 0o755, 0, 0, PREP.encode())

    files = stage / 'bootstrap/files'
    shutil.rmtree(files)
    files.mkdir()
    manifest, count = [], 0
    order = {'d': 0, 'f': 1, 'l': 2}
    for name, (kind, mode, uid, gid, body) in sorted(
            entries.items(), key=lambda kv: (order[kv[1][0]], len(PurePosixPath(kv[0]).parts), kv[0])):
        line = [kind, '%o' % mode, str(uid), str(gid), '/private/' + name]
        if kind == 'f':
            count += 1
            (files / str(count)).write_bytes(body)
            line.append('/redwood/bootstrap/files/%d' % count)
        elif kind == 'l':
            line.append(body)
        manifest.append('\t'.join(line))
    for marker in ('.procursus_strapped', '.installed_dopamine'):
        if 'var/jb/' + marker not in entries:
            count += 1
            (files / str(count)).write_bytes(b'')
            manifest.append('\t'.join(['f', '644', '0', '0', '/private/var/jb/' + marker,
                                       '/redwood/bootstrap/files/%d' % count]))
    (stage / 'bootstrap/manifest').write_text('\n'.join(manifest) + '\n')

    gpu_firmware = ROOT / 'artifacts/ace6t/gpu-android'
    for name in ('gen80900_gmu.bin', 'gen80900_sqe.fw', 'gen80900_aqe.fw', 'gen80900_zap.mbn'):
        if (gpu_firmware / name).exists():
            (stage / 'fw').mkdir(exist_ok=True)
            shutil.copy2(gpu_firmware / name, stage / 'fw' / name)
    wifi_firmware = ROOT / 'artifacts/ace6t/wifi-android/fw'
    if wifi_firmware.is_dir():
        (stage / 'fw').mkdir(exist_ok=True)
        for source in sorted(wifi_firmware.iterdir()):
            shutil.copy2(source, stage / 'fw' / source.name)
            if source.name == 'network':
                (stage / 'fw' / source.name).chmod(0o600)
    shutil.copy2(ASSETS / 'bootstrap-copy', stage / 'bin/bootstrap-copy')
    for lib in ('libredwood-sb.dylib', 'libredwood-tweaks.dylib', 'libredwood-app.dylib'):
        source = ROOT / 'userland/obj' / lib
        if not source.exists():
            raise SystemExit('missing %s: build it (userland/build.sh)' % source)
        shutil.copy2(source, stage / 'lib' / lib)
    (stage / 'etc/rc').write_text(RC)
    inject = [line for line in (stage / 'etc/inject.conf').read_text().splitlines()
              if line.strip() and not any(line.split()[0] == want.split()[0] for want in INJECT)]
    (stage / 'etc/inject.conf').write_text('\n'.join(inject + INJECT) + '\n')
    print('sileo_extras: %d entries, %d files, %d links' % (
        len(manifest), count, sum(1 for line in manifest if line.startswith('l\t'))))

    if args.pack:
        subprocess.run(['python3', str(ROOT / 'tools/mkpackfs.py'), '--zstd', '15', '--raw-prefix', 'fw',
                        '-o', args.pack, str(stage)], check=True)

if __name__ == '__main__':
    main()
