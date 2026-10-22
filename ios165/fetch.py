#!/usr/bin/env python3
import importlib.util
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
WINDOWS = os.name == 'nt'
ROOTFS_DMG = '096-04088-077.dmg'
CRYPTEX_DMG = '096-04666-078.dmg'
APP_CRYPTEX_DMG = '096-03536-083.dmg'
CACHE = '20F66__iPhone10,3_6'
KEEP = bool(os.environ.get('KEEP'))

spec = importlib.util.spec_from_file_location('select_system', HERE / 'select-system.py')
select_system = importlib.util.module_from_spec(spec)
spec.loader.exec_module(select_system)


def say(text):
    print('fetch: ' + text, flush=True)


def die(text):
    sys.exit('fetch: ' + text)


def find_7z():
    candidates = [os.environ.get('SEVENZIP'), shutil.which('7z'), shutil.which('7zz')]
    if WINDOWS:
        candidates += [r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe',
                       str(ROOT / 'work' / 'kit' / 'tools' / '7z.exe')]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    die('7-Zip (23 or newer) is needed: ' +
        ('winget install -e --id 7zip.7zip' if WINDOWS else 'install p7zip-full or 7zip'))


def find_unzbm():
    if WINDOWS:
        for candidate in (ROOT / 'work' / 'kit' / 'tools' / 'unzbm.exe', ROOT / 'work' / 'tools' / 'unzbm.exe'):
            if candidate.is_file():
                return str(candidate)
        die('unzbm.exe is missing: run --get first')
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
        with open(HERE / log, 'w') as out:
            result = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT, cwd=HERE)
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
    with open(HERE / out, 'w', errors='replace') as f:
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
    with open(HERE / out, 'w') as manifest:
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
        local = HERE / folder / path[len(base):]
        if local.exists():
            paths.append(str(local))
    for at in range(0, len(paths), 100):
        subprocess.run([UNZBM] + paths[at:at + 100], check=True)
    left = []
    if WINDOWS:
        for path, size in sizes.items():
            if base and not path.startswith(base):
                continue
            local = HERE / folder / path[len(base):]
            if local.exists() and size and local.stat().st_size != size:
                left.append(str(local))
    else:
        for dirpath, _, files in os.walk(HERE / folder):
            left += [os.path.join(dirpath, f) for f in files if ':com.apple.' in f]
    if left:
        die(f'{len(left)} files in {folder} are still compressed, e.g. {left[0]}')


def fetch_rootfs():
    done = HERE / '.done-rootfs'
    if done.exists() and done.stat().st_mtime >= (HERE / 'select-system.py').stat().st_mtime:
        return
    download(ROOTFS_DMG, 'rootfs.dmg')
    say('listing the root filesystem')
    listing('rootfs.dmg', 'rootfs.slt')
    subprocess.run([sys.executable, str(HERE / 'select-system.py'), 'rootfs.slt', 'system.files', 'system.manifest'], check=True, cwd=HERE)
    with open(HERE / 'var.skel', 'w') as out:
        subprocess.run([sys.executable, str(HERE / 'mkvarskel.py'), 'rootfs.slt'], stdout=out, check=True, cwd=HERE)
    count = sum(1 for _ in open(HERE / 'system.files'))
    say(f'extracting {count} files')
    shutil.rmtree(HERE / 'rootfs', ignore_errors=True)
    seven('x', '-y', '-orootfs', 'rootfs.dmg', '@system.files', *(['-sns'] if WINDOWS else []), log='extract-system.log')
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
        return
    download(CRYPTEX_DMG, 'cryptex-os.dmg')
    say('extracting the OS cryptex')
    listing('cryptex-os.dmg', 'cryptex.slt', streams=True)
    for folder in ('cryptex', 'cryptex.cache', CACHE):
        shutil.rmtree(HERE / folder, ignore_errors=True)
    streams = ['-sns'] if WINDOWS else []
    seven('x', '-y', '-ocryptex', 'cryptex-os.dmg', '-xr!com.apple.dyld', '-x!System/DriverKit', *streams,
          log='extract-cryptex.log', check=False)
    sizes = compressed('cryptex.slt')
    unzbm_all('cryptex', {p: s for p, s in sizes.items() if '/com.apple.dyld/' not in p and not p.startswith('System/DriverKit')})
    seven('x', '-y', '-ocryptex.cache', 'cryptex-os.dmg', 'System/Library/Caches/com.apple.dyld', '-xr!*.symbols*', *streams,
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
        return
    download(APP_CRYPTEX_DMG, 'cryptex-app.dmg')
    say('extracting the App cryptex')
    listing('cryptex-app.dmg', 'cryptex-app.slt', streams=True)
    shutil.rmtree(HERE / 'cryptex-app', ignore_errors=True)
    seven('x', '-y', '-ocryptex-app', 'cryptex-app.dmg', *(['-sns'] if WINDOWS else []), log='extract-cryptex-app.log', check=False)
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
    main()
