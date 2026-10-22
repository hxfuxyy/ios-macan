#!/usr/bin/env python3
import importlib.util
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ROOTFS_DMG = '096-04088-077.dmg'
CRYPTEX_DMG = '096-04666-078.dmg'
APP_CRYPTEX_DMG = '096-03536-083.dmg'
CACHE = '20F66__iPhone10,3_6'
KEEP = bool(os.environ.get('KEEP'))
os.environ.setdefault('PYTHONUTF8', '1')

spec = importlib.util.spec_from_file_location('select_system', HERE / 'select-system.py')
select_system = importlib.util.module_from_spec(spec)
spec.loader.exec_module(select_system)


def say(text):
    print('fetch: ' + text, flush=True)


def die(text):
    sys.exit('fetch: ' + text)


SEVENZIP_URL = 'https://github.com/ip7z/7zip/releases/download/26.05/'
LOCAL_7Z = ROOT / 'work' / '7zip'


def seven_version(path):
    try:
        out = subprocess.run([path], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    for line in out.splitlines():
        if '7-Zip' in line:
            found = re.search(r'(\d+)\.(\d+)', line)
            return int(found.group(1)) if found else 0
    return 0


def local_7z():
    return LOCAL_7Z / '7zz'


def get_7z():
    import tarfile
    sys.path.insert(0, str(ROOT / 'scripts'))
    from hackios_tool import download as get_file
    shutil.rmtree(LOCAL_7Z, ignore_errors=True)
    LOCAL_7Z.mkdir(parents=True)
    arm = platform.machine().lower() in ('aarch64', 'arm64')
    package = LOCAL_7Z / '7z.tar.xz'
    get_file(SEVENZIP_URL + ('7z2605-linux-arm64.tar.xz' if arm else '7z2605-linux-x64.tar.xz'), package, '7-Zip')
    with tarfile.open(package) as archive:
        archive.extract('7zz', LOCAL_7Z)
    package.unlink()
    local_7z().chmod(0o755)


def find_7z():
    candidates = [os.environ.get('SEVENZIP'), str(local_7z()), shutil.which('7zz'), shutil.which('7z')]
    old = []
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            version = seven_version(candidate)
            if version >= 23:
                return candidate
            old.append(f'{candidate} ({version or "unknown version"})')
    say('7-Zip 23 or newer is needed to open iOS images' + (f'; too old here: {", ".join(old)}' if old else '') +
        ': downloading the official 7-Zip into work/7zip')
    get_7z()
    if seven_version(str(local_7z())) >= 23:
        return str(local_7z())
    die('7-Zip could not be set up: install 7-Zip 23 or newer from https://www.7-zip.org/ and run this again')


def find_unzbm():
    binary = ROOT / 'xnu-deps' / 'host' / 'bin' / 'unzbm'
    kit = ROOT / 'work' / 'kit' / 'tools' / 'unzbm'
    if binary.is_file():
        return str(binary)
    if kit.is_file():
        return str(kit)
    source = ROOT / 'xnu-deps' / 'build' / 'lzbitmap'
    binary.parent.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        die('a C compiler is needed to build unzbm')
    subprocess.run([compiler, '-O2', '-w', '-o', str(binary), str(source / 'unzbm.c'), str(source / 'libzbitmap.c')], check=True)
    return str(binary)


SEVENZIP = UNZBM = None


def seven(*args, log=None, check=True):
    command = [SEVENZIP] + list(args)
    if log:
        with open(HERE / log, 'w', encoding='utf-8', errors='replace') as out:
            result = subprocess.run(command + ['-bsp2', '-bse1'], stdout=out, cwd=HERE)
    else:
        result = subprocess.run(command, cwd=HERE)
    if check and result.returncode not in (0, 1, 2):
        die(f'7z failed ({result.returncode}): ' + ' '.join(args))
    return result.returncode


def download(member, target):
    if (HERE / target).is_file():
        return
    say(f'downloading {target} from Apple')
    subprocess.run([sys.executable, str(HERE / 'fetch-from-ipsw.py'), member, target + '.part'], check=True, cwd=HERE)
    os.replace(HERE / (target + '.part'), HERE / target)


def listing(dmg, out, streams=False):
    with open(HERE / out, 'w', encoding='utf-8', errors='replace') as f:
        subprocess.run([SEVENZIP, 'l', '-slt'] + (['-sns'] if streams else []) + [dmg], stdout=f, cwd=HERE, check=True)


def compressed(listing_file):
    found = {}
    for entry in select_system.entries(HERE / listing_file):
        path = entry['Path']
        if path.endswith(':com.apple.decmpfs'):
            found[path[:-len(':com.apple.decmpfs')]] = None
    sizes = {}
    for entry in select_system.entries(HERE / listing_file):
        if entry['Path'] in found:
            sizes[entry['Path']] = int(entry.get('Size') or 0)
    return sizes


def manifest_from(listing_file, out, strip='', prefix=''):
    with open(HERE / out, 'w', encoding='utf-8', newline='\n') as manifest:
        for entry in select_system.entries(HERE / listing_file):
            path = entry['Path']
            if ':' in path:
                continue
            mode = select_system.mode_bits(entry['Mode'])
            if mode is None or not path.startswith(strip):
                continue
            rel = prefix + path[len(strip):]
            target = entry.get('Symbolic Link', '') if stat.S_ISLNK(mode) else ''
            manifest.write(f'{mode:o}\t{entry.get("User ID") or 0}\t{entry.get("Group ID") or 0}\t{rel}\t{target}\n')


def unzbm_all(folder, sizes, base=''):
    paths = []
    for path, size in sizes.items():
        if base and not path.startswith(base):
            continue
        local = str(HERE / folder / path[len(base):])
        if os.path.exists(local):
            paths.append(local)
    if paths:
        say(f'decompressing {len(paths)} files')
    for at in range(0, len(paths), 100):
        subprocess.run([UNZBM] + paths[at:at + 100], check=True)
    left = []
    for dirpath, _, files in os.walk(HERE / folder):
        left += [os.path.join(dirpath, f) for f in files if ':com.apple.' in f]
    if left:
        die(f'{len(left)} files in {folder} are still compressed, e.g. {left[0]}')


def fetch_rootfs():
    done = HERE / '.done-rootfs'
    if done.exists() and done.stat().st_mtime >= (HERE / 'select-system.py').stat().st_mtime:
        say('[1/3] the root filesystem: already there')
        return
    say('[1/3] the root filesystem')
    download(ROOTFS_DMG, 'rootfs.dmg')
    say('listing the root filesystem')
    listing('rootfs.dmg', 'rootfs.slt')
    subprocess.run([sys.executable, str(HERE / 'select-system.py'), 'rootfs.slt', 'system.files', 'system.manifest'], check=True, cwd=HERE)
    with open(HERE / 'var.skel', 'w', encoding='utf-8') as out:
        subprocess.run([sys.executable, str(HERE / 'mkvarskel.py'), 'rootfs.slt'], stdout=out, check=True, cwd=HERE)
    count = sum(1 for _ in open(HERE / 'system.files', encoding='utf-8'))
    if count < 10000:
        die(f'only {count} files chosen from the root filesystem listing (ios165/rootfs.slt): the listing is not what 7-Zip should print')
    say(f'extracting {count} files (a few minutes)')
    shutil.rmtree(HERE / 'rootfs', ignore_errors=True)
    seven('x', '-y', '-orootfs', 'rootfs.dmg', '@system.files', log='extract-system.log')
    for need in ('System/Library/CoreServices/SpringBoard.app/SpringBoard', 'usr/lib/dyld', 'usr/libexec/backboardd'):
        if not (HERE / 'rootfs' / need).is_file():
            die(f'the root filesystem is incomplete (no {need}): see ios165/extract-system.log, then run this again')
    trim = ROOT / 'tools' / 'font_trim.py'
    emoji = 'rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc'
    pingfang = 'rootfs/System/Library/Fonts/LanguageSupport/PingFang.ttc'
    if (HERE / emoji).exists():
        subprocess.run([sys.executable, str(trim), 'emoji', emoji, emoji, '40,64,96'], check=True, cwd=HERE)
    if (HERE / pingfang).exists():
        subprocess.run([sys.executable, str(trim), 'faces', pingfang, pingfang, r'^\.?PingFang(SC|TC|HK)-(Regular|Semibold)$'], check=True, cwd=HERE)
    done.touch()
    if not KEEP:
        (HERE / 'rootfs.dmg').unlink(missing_ok=True)


def fetch_cryptex():
    if (HERE / '.done-cryptex').exists():
        say('[2/3] the OS cryptex: already there')
        return
    say('[2/3] the OS cryptex')
    download(CRYPTEX_DMG, 'cryptex-os.dmg')
    say('extracting the OS cryptex (a few minutes)')
    listing('cryptex-os.dmg', 'cryptex.slt', streams=True)
    for folder in ('cryptex', 'cryptex.cache', CACHE):
        shutil.rmtree(HERE / folder, ignore_errors=True)
    seven('x', '-y', '-ocryptex', 'cryptex-os.dmg', '-xr!com.apple.dyld', '-x!System/DriverKit',
          log='extract-cryptex.log', check=False)
    sizes = compressed('cryptex.slt')
    unzbm_all('cryptex', {p: s for p, s in sizes.items() if '/com.apple.dyld/' not in p and not p.startswith('System/DriverKit')})
    seven('x', '-y', '-ocryptex.cache', 'cryptex-os.dmg', 'System/Library/Caches/com.apple.dyld', '-xr!*.symbols*',
          log='extract-dyld.log', check=False)
    dyld = 'System/Library/Caches/com.apple.dyld/'
    unzbm_all('cryptex.cache', {p: s for p, s in sizes.items() if p.startswith(dyld) and '.symbols' not in p})
    os.replace(HERE / 'cryptex.cache' / 'System' / 'Library' / 'Caches' / 'com.apple.dyld', HERE / CACHE)
    shutil.rmtree(HERE / 'cryptex.cache', ignore_errors=True)
    if len(os.listdir(HERE / CACHE)) != 43:
        die('the dyld shared cache is incomplete')
    manifest_from('cryptex.slt', 'cryptex.manifest')
    (HERE / '.done-cryptex').touch()
    if not KEEP:
        (HERE / 'cryptex-os.dmg').unlink(missing_ok=True)


def fetch_app_cryptex():
    if (HERE / '.done-cryptex-app').exists():
        say('[3/3] the App cryptex: already there')
        return
    say('[3/3] the App cryptex')
    download(APP_CRYPTEX_DMG, 'cryptex-app.dmg')
    say('extracting the App cryptex')
    listing('cryptex-app.dmg', 'cryptex-app.slt', streams=True)
    shutil.rmtree(HERE / 'cryptex-app', ignore_errors=True)
    seven('x', '-y', '-ocryptex-app', 'cryptex-app.dmg', log='extract-cryptex-app.log', check=False)
    unzbm_all('cryptex-app', compressed('cryptex-app.slt'))
    if not (HERE / 'cryptex-app' / 'usr' / 'libexec' / 'webbookmarksd').is_file():
        die('the App cryptex is incomplete')
    manifest_from('cryptex-app.slt', 'cryptex-app.manifest')
    (HERE / '.done-cryptex-app').touch()


def main():
    global SEVENZIP, UNZBM
    SEVENZIP = find_7z()
    UNZBM = find_unzbm()
    fetch_rootfs()
    fetch_cryptex()
    fetch_app_cryptex()
    say('done: iOS 16.5 is in ios165/')


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        die(f'{Path(str(error.cmd[1] if error.cmd[0] == sys.executable else error.cmd[0])).name} stopped with code {error.returncode}')
    except KeyboardInterrupt:
        die('stopped (Ctrl+C)')
    except OSError as error:
        die(f'{error}')
