#!/usr/bin/env python3
import argparse
import os
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import mkpackfs as pf

IOS = ROOT / 'ios165'
A = ROOT / 'artifacts' / 'ace6t'
CACHE = '20F66__iPhone10,3_6'
JOBS = '''com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
com.apple.powerd com.apple.analyticsd com.apple.logd_helper
com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
com.apple.SpringBoard com.apple.wifid'''.split()


def say(text):
    print('assemble: ' + text, flush=True)


def timezone():
    zone = os.environ.get('TZ_NAME')
    if zone:
        return zone
    if os.name != 'nt' and shutil.which('timedatectl'):
        out = subprocess.run(['timedatectl', 'show', '-p', 'Timezone', '--value'], capture_output=True, text=True).stdout.strip()
        if out:
            return out
    try:
        link = os.readlink('/etc/localtime')
        if 'zoneinfo/' in link:
            return link.split('zoneinfo/', 1)[1]
    except OSError:
        pass
    return 'UTC'


def var_skel(ours):
    lines = (IOS / 'var.skel').read_text().splitlines()
    zone = timezone()
    out = []
    for line in lines:
        if line == 'l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo':
            line = 'l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo'
        elif line.startswith('l\t/var/db/timezone/zoneinfo/') and line.endswith('\t/private/var/db/timezone/localtime'):
            line = f'l\t/var/db/timezone/zoneinfo/{zone}\t/private/var/db/timezone/localtime'
        out.append(line)
    return '\n'.join(out) + '\n' + ours


def root_image(kit, output, scratch):
    say('root image')
    root = pf.scan_tar(str(kit / 'root-base.tar'), str(scratch / 'base'))
    ours_node = pf.node_at(root, 'etc/var.skel.redwood')
    ours = Path(ours_node.source).read_text() if ours_node else ''
    pf.remove(root, 'etc/var.skel.redwood')
    skel = scratch / 'var.skel'
    skel.write_text(var_skel(ours))
    pf.add_file(root, 'etc/var.skel', str(skel))

    jobs = scratch / 'jobs'
    jobs.mkdir()
    env = dict(os.environ, OSLOG=os.environ.get('OSLOG', 'com.apple.backboardd'))
    subprocess.run([sys.executable, str(ROOT / 'userland' / 'mkjobs.py'), '--rest', str(IOS / 'rootfs'), str(jobs)] + JOBS,
                   check=True, env=env)
    wifid_job = plistlib.loads((IOS / 'rootfs/System/Library/LaunchDaemons/com.apple.wifid.plist').read_bytes())
    wifid_job['Program'] = '/redwood/bin/wifid'
    if 'ProgramArguments' in wifid_job:
        wifid_job['ProgramArguments'][0] = '/redwood/bin/wifid'
    wifid_job.pop('_LimitLoadToDeviceTree', None)
    wifid_job['StandardOutPath'] = wifid_job['StandardErrorPath'] = '/dev/console'
    wifid_job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    (jobs / 'com.apple.wifid.plist').write_bytes(plistlib.dumps(wifid_job))
    for job in sorted(jobs.iterdir()):
        pf.add_file(root, f'LaunchDaemons/{job.name}', str(job))
    pf.add_file(root, 'bin/wifid', str(IOS / 'rootfs/usr/sbin/wifid'), 0o100755)
    for name in ('extensions.plist', 'extensions.plist.sig'):
        source = IOS / 'rootfs/System/Library/xpc' / name
        if source.is_file():
            pf.add_file(root, f'xpc/{name}', str(source))

    firmware = 0
    for folder, names in ((A / 'gpu-android', ('gen80900_gmu.bin', 'gen80900_sqe.fw', 'gen80900_aqe.fw', 'gen80900_zap.mbn')),
                          (A / 'wifi-android' / 'fw', None), (A / 'hw-android' / 'fw', ('aw8697_haptic_170.bin',))):
        if not folder.is_dir():
            continue
        for source in sorted(folder.iterdir()):
            if source.is_file() and (names is None or source.name in names):
                pf.add_file(root, f'fw/{source.name}', str(source), 0o100600 if source.name == 'network' else 0o100644)
                firmware += 1
    if not pf.node_at(root, 'fw/network'):
        say('no Wi-Fi network set (--wifi NAME PASS): iOS starts without Wi-Fi')
    say(f'{firmware} firmware files')
    pf.pack(root, str(output), zstd=15, raw=pf.sources_under(root, 'fw'))


def system_image(kit, output, scratch):
    say('system image (a few minutes)')
    root, skipped = pf.scan(os.path.realpath(IOS / 'rootfs'))
    for path in ('System/Library/LaunchDaemons', 'System/Library/NanoLaunchDaemons', 'System/Library/LaunchAngels',
                 'System/Library/xpc/launchd.plist'):
        pf.remove(root, path)
    pf.add_symlink(root, 'System/Library/LaunchDaemons', '/redwood/LaunchDaemons')
    for path in ('redwood', 'sbin', 'System/Volumes/iOS'):
        pf.node_at(root, path, create=True)
    staged = pf.node_at(root, 'private/var/staged_system_apps')
    if staged:
        for name in list(staged.children):
            pf.move(root, f'private/var/staged_system_apps/{name.decode()}', f'Applications/{name.decode()}')
        pf.remove(root, 'private/var')
    pf.move(root, 'sbin/launchd', 'sbin/launchd.ios')
    overlay = pf.scan_tar(str(kit / 'system-overlay.tar'), str(scratch / 'overlay'))
    for name, child in list(overlay.children.items()):
        pf.graft(root, name.decode(), child)
    adsp = A / 'hw-android' / 'adsp'
    if (adsp / 'adsp.mdt').is_file():
        sys.path.insert(0, str(ROOT / 'tools'))
        from macan_adsp_firmware import check
        check(adsp)
        for source in sorted(set(adsp.glob('adsp*.mdt')) | set(adsp.glob('adsp*.b[0-9][0-9]'))):
            pf.add_file(root, f'fw/{source.name}', str(source))
    pf.add_symlink(root, 'System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver', '/redwood/lib/RedwoodAudio.driver')
    pf.node_at(root, 'System/Cryptexes', create=True)
    for folder, place in (('cryptex', 'OS'), ('cryptex-app', 'App')):
        if (IOS / folder).is_dir():
            tree, more = pf.scan(os.path.realpath(IOS / folder))
            skipped += more
            pf.graft(root, f'System/Cryptexes/{place}', tree)
    dyld = 'System/Cryptexes/OS/System/Library/Caches/com.apple.dyld'
    pf.node_at(root, dyld, create=True)
    for source in sorted((IOS / CACHE).iterdir()):
        if '.symbols' not in source.name:
            pf.add_file(root, f'{dyld}/{source.name}', str(source), 0o100755)
    pf.apply_manifest(root, str(IOS / 'system.manifest'))
    for name, place in (('cryptex.manifest', 'System/Cryptexes/OS/'), ('cryptex-app.manifest', 'System/Cryptexes/App/')):
        if (IOS / name).is_file():
            prefixed = scratch / name
            with open(IOS / name, 'rb') as src, open(prefixed, 'wb') as dst:
                for line in src:
                    fields = line.split(b'\t')
                    if len(fields) >= 4:
                        fields[3] = place.encode() + fields[3]
                        dst.write(b'\t'.join(fields))
            pf.apply_manifest(root, str(prefixed))
    pf.pack(root, str(output), zstd=15, raw=pf.sources_under(root, 'fw'))


def main():
    parser = argparse.ArgumentParser(description='Make the root and system images from the kit, iOS and the firmware.')
    parser.add_argument('--kit', type=Path, default=ROOT / 'work' / 'kit')
    parser.add_argument('--out', type=Path, default=ROOT / 'work' / 'images')
    parser.add_argument('--only', choices=('root', 'system'))
    args = parser.parse_args()
    for need in ('root-base.tar', 'system-overlay.tar'):
        if not (args.kit / need).is_file():
            sys.exit(f'assemble: {args.kit / need} is missing: run --get or --build')
    if not (IOS / 'rootfs').is_dir() or not (IOS / CACHE).is_dir():
        sys.exit('assemble: iOS is not in ios165/: run --fetch')
    args.out.mkdir(parents=True, exist_ok=True)
    started = time.time()
    with tempfile.TemporaryDirectory(dir=args.out) as scratch:
        scratch = Path(scratch)
        if args.only in (None, 'root'):
            root_image(args.kit, args.out / 'root.packfs', scratch)
        if args.only in (None, 'system'):
            system_image(args.kit, args.out / 'system.packfs', scratch)
    say(f'done in {int(time.time() - started)} s: {args.out}')


if __name__ == '__main__':
    main()
