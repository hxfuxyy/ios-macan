#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

if sys.version_info < (3, 8):
    sys.exit('hackios: Python 3.8 or newer is needed (this is ' + sys.version.split()[0] + ')')
ROOT = Path(__file__).resolve().parents[1]
KIT = ROOT / 'work' / 'kit'
IMAGES = ROOT / 'work' / 'images'
RELEASE = 'https://github.com/hxfuxyy/ios-macan/releases/latest/download/hackios-kit.zip'
LOG = ROOT / 'work' / 'logs' / 'hackios.log'
os.environ['PYTHONUTF8'] = '1'
os.environ['PYTHONIOENCODING'] = 'utf-8'
BOLD, GREEN, RED, RESET = ('\033[1;36m', '\033[1;32m', '\033[1;31m', '\033[0m') if sys.stdout.isatty() else ('',) * 4


def say(text):
    print(f'{BOLD}hackios:{RESET} {text}', flush=True)


def die(text):
    print(f'{RED}hackios:{RESET} {text}', file=sys.stderr, flush=True)
    sys.exit(1)


def step(number, total, text):
    print(f'\n{BOLD}[{number}/{total}] {text}{RESET}', flush=True)


def megabytes(count):
    return f'{count / 1048576:,.0f} MB'


def show_progress(label, done, total, start):
    elapsed = max(time.time() - start, 0.001)
    speed = done / elapsed
    if total:
        fraction = min(done / total, 1)
        filled = int(fraction * 30)
        left = (total - done) / speed if speed > 0 else 0
        line = (f'  {label} [{"#" * filled}{"." * (30 - filled)}] {fraction * 100:5.1f}%  '
                f'{megabytes(done)} of {megabytes(total)}  {speed / 1048576:4.1f} MB/s  {int(left // 60)}:{int(left % 60):02d} left ')
    else:
        line = f'  {label} {megabytes(done)}  {speed / 1048576:4.1f} MB/s '
    print('\r' + line, end='', flush=True)


def download(url, path, label):
    import urllib.error
    import urllib.request
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(url, timeout=60) as response, open(path, 'wb') as out:
            total = int(response.headers.get('Content-Length') or 0)
            done, start, shown = 0, time.time(), 0
            while True:
                block = response.read(1 << 20)
                if not block:
                    break
                out.write(block)
                done += len(block)
                if time.time() - shown >= 0.3:
                    shown = time.time()
                    show_progress(label, done, total, start)
            show_progress(label, done, total, start)
            print(flush=True)
    except (urllib.error.URLError, OSError) as error:
        print(flush=True)
        path.unlink(missing_ok=True)
        die(f'download failed ({getattr(error, "reason", error)}): check the Internet connection and run this again')
    if total and done != total:
        path.unlink(missing_ok=True)
        die(f'the download stopped at {megabytes(done)} of {megabytes(total)}: run this again')


def run_python(script, what):
    code = subprocess.call([sys.executable, str(ROOT / script)])
    if code:
        die(f'{what} failed (the reason is in the lines above). Fix it and run the same command again: '
            'finished parts are kept.')


PLATFORM_TOOLS = ROOT / 'work' / 'platform-tools'


def get_platform_tools():
    import zipfile
    system = 'darwin' if sys.platform == 'darwin' else 'linux'
    archive = ROOT / 'work' / 'platform-tools.zip'
    say("downloading Google's platform-tools (adb, fastboot) into work/platform-tools")
    download(f'https://dl.google.com/android/repository/platform-tools-latest-{system}.zip', archive, 'platform-tools.zip')
    shutil.rmtree(PLATFORM_TOOLS, ignore_errors=True)
    with zipfile.ZipFile(archive) as tools_zip:
        tools_zip.extractall(ROOT / 'work')
    archive.unlink()
    for path in PLATFORM_TOOLS.iterdir():
        if path.is_file() and '.' not in path.name:
            path.chmod(0o755)
    if not (PLATFORM_TOOLS / 'fastboot').is_file():
        die('platform-tools did not unpack: get them from https://developer.android.com/tools/releases/platform-tools')


def find_tool(name):
    local = PLATFORM_TOOLS / name
    if local.is_file():
        return str(local)
    found = shutil.which(name)
    if found:
        return found
    get_platform_tools()
    return str(local)


def usb_trouble():
    _, out = call([FASTBOOT, 'devices'], 5)
    if 'no permissions' in out:
        die('fastboot has no permission to use the phone: run  ./hackios --udev  once, replug the cable, then --boot again')
    _, out = call([ADB, 'devices'], 5)
    if 'no permissions' in out:
        die('adb has no permission to use the phone: run  ./hackios --udev  once, replug the cable, then retry')
    _, out = call([ADB, 'devices'], 5)
    if '\tunauthorized' in out:
        say('the phone asks to allow USB debugging: unlock it and press Allow')


ADB = FASTBOOT = None


def tools():
    global ADB, FASTBOOT
    if not ADB:
        ADB, FASTBOOT = find_tool('adb'), find_tool('fastboot')
        try:
            subprocess.run([ADB, 'start-server'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60)
        except (OSError, subprocess.SubprocessError):
            pass


def call(command, timeout=10):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return result.returncode, (result.stdout or '') + (result.stderr or '')
    except (subprocess.TimeoutExpired, OSError):
        return 1, ''


def fastboot_devices():
    _, out = call([FASTBOOT, 'devices'], 5)
    return [line.split()[0] for line in out.splitlines() if line.strip().endswith('fastboot')]


def in_loader():
    return 'xnuloader' in fastboot_devices()


def firmware_fastboot():
    return [d for d in fastboot_devices() if d != 'xnuloader']


def adb_ready():
    _, out = call([ADB, 'devices'], 5)
    return any(line.strip().endswith('\tdevice') for line in out.splitlines()[1:])


def wait_for(seconds, what, check):
    deadline = time.time() + seconds
    checked = time.time()
    while not check():
        if time.time() > deadline:
            usb_trouble()
            die(f'timeout after {seconds} s waiting for {what}')
        if time.time() - checked > 15:
            checked = time.time()
            usb_trouble()
        time.sleep(2)


def boot_uefi():
    tools()
    path = KIT / 'uefi.img'
    if not path.is_file():
        die('no UEFI image: run --get (or --build)')
    if not fastboot_devices() and not adb_ready():
        usb_trouble()
        die('no phone over USB (neither fastboot nor adb): connect it and retry')
    if len(firmware_fastboot()) > 1:
        die('more than one phone in fastboot: connect only one')
    if in_loader():
        say('the phone is in the loader: back to Android first')
        call([FASTBOOT, '-s', 'xnuloader', 'reboot'], 15)
        time.sleep(5)
    if not firmware_fastboot():
        wait_for(240, 'Android over adb', adb_ready)
        wait_for(180, 'Android to finish booting',
                 lambda: call([ADB, 'shell', 'getprop', 'sys.boot_completed'], 5)[1].strip() == '1')
        say('Android is up: rebooting into the bootloader')
        call([ADB, 'reboot', 'bootloader'], 15)
        wait_for(120, 'the bootloader (fastboot)', lambda: bool(firmware_fastboot()))
    serial = firmware_fastboot()[0]
    say('fastboot boot uefi.img (RAM only, nothing is flashed)')
    code, out = call([FASTBOOT, '-s', serial, 'boot', str(path)], 60)
    if code:
        die('fastboot boot failed:\n' + out)
    wait_for(180, 'the iOS loader', in_loader)
    say('the loader is up')


def run_ios(options):
    for need in (KIT / 'kernel', IMAGES / 'root.packfs', IMAGES / 'system.packfs'):
        if not need.is_file():
            die(f'{need.relative_to(ROOT)} is missing: run --get (or --build)')
    command = [sys.executable, str(ROOT / 'scripts' / 'run_ios.py'), '--kernel', str(KIT / 'kernel'),
               '--root-image', str(IMAGES / 'root.packfs'), '--system-image', str(IMAGES / 'system.packfs')]
    sys.exit(subprocess.call(command + options))


def latest_kit_id():
    import json
    import urllib.request
    try:
        with urllib.request.urlopen('https://api.github.com/repos/hxfuxyy/ios-macan/releases/latest', timeout=30) as response:
            release = json.load(response)
        return next(str(asset['id']) + ' ' + asset['updated_at'] for asset in release['assets'] if asset['name'] == 'hackios-kit.zip')
    except Exception:
        return ''


def get_kit():
    import zipfile
    KIT.mkdir(parents=True, exist_ok=True)
    archive = ROOT / 'work' / 'hackios-kit.zip'
    stamp = KIT / 'RELEASE'
    latest = latest_kit_id()
    complete = all((KIT / name).exists() for name in ('kernel', 'uefi.img', 'root-base.tar', 'system-overlay.tar', 'tools'))
    if latest and complete and stamp.is_file() and stamp.read_text().strip() == latest:
        say('the release kit is up to date')
        return
    say('downloading the release kit (kernel, UEFI, userland) from GitHub')
    download(RELEASE, archive, 'hackios-kit.zip')
    say('unpacking the kit')
    try:
        with zipfile.ZipFile(archive) as kit:
            kit.extractall(KIT)
    except zipfile.BadZipFile:
        archive.unlink(missing_ok=True)
        die('the downloaded kit is damaged: run this again')
    archive.unlink()
    if latest:
        stamp.write_text(latest + '\n')
    path = KIT / 'tools' / 'unzbm'
    if path.exists():
        path.chmod(0o755)
    say('kit ready in work/kit: ' + ', '.join(sorted(p.name for p in KIT.iterdir())))


def fetch_ios():
    run_python('ios165/fetch.py', 'getting iOS 16.5')


def assemble():
    run_python('tools/assemble.py', 'making the images')


def wifi(words):
    path = ROOT / 'work' / 'wifi-network'
    if words == ['off']:
        path.unlink(missing_ok=True)
        say('no network is joined by itself any more')
    elif len(words) == 2:
        ssid, password = words
        if not 1 <= len(ssid.encode()) <= 32:
            die('the network name must be 1 to 32 bytes long')
        if not 8 <= len(password) <= 63:
            die('the password must be 8 to 63 characters (WPA2); open networks are not supported here')
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as out:
            out.write(f'{ssid}\n{password}\n')
        path.chmod(0o600)
        say(f'iOS will join "{ssid}" by itself at every start')
    else:
        die('usage: --wifi "NETWORK NAME" "PASSWORD"   or   --wifi off')
    if (KIT / 'root-base.tar').is_file() and (ROOT / 'ios165' / 'rootfs').is_dir():
        result = subprocess.run([sys.executable, str(ROOT / 'tools' / 'assemble.py'), '--only', 'root'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            print(result.stdout, end='')
            die('the root image could not be made again: run --assemble')
        say('the root image is made again: now --boot')
    else:
        say('it goes into the images at --get')


def android():
    tools()
    if in_loader():
        call([FASTBOOT, '-s', 'xnuloader', 'reboot'], 15)
        say('rebooting into Android')
    elif firmware_fastboot():
        call([FASTBOOT, 'reboot'], 15)
        say('rebooting into Android')
    else:
        say('the phone is not in the loader or fastboot')


def status():
    tools()
    _, adb_out = call([ADB, 'devices'], 5)
    phones = [line.split()[0] for line in adb_out.splitlines()[1:] if line.strip().endswith('device')]
    print('phone in Android:  ', ', '.join(phones) or 'no')
    print('phone in fastboot: ', ', '.join(fastboot_devices()) or 'no')
    print('iOS 16.5 fetched:  ', 'yes' if (ROOT / 'ios165' / 'rootfs').is_dir() else 'no')
    print('kit (UEFI, kernel):', 'yes' if (KIT / 'uefi.img').is_file() and (KIT / 'kernel').is_file() else 'no')
    print('root image:        ', 'yes' if (IMAGES / 'root.packfs').is_file() else 'no')
    print('system image:      ', 'yes' if (IMAGES / 'system.packfs').is_file() else 'no')
    print('Wi-Fi network:     ', 'set' if (ROOT / 'work' / 'wifi-network').is_file() else 'not set (choose it in Settings)')


def main(argv):
    if not argv:
        die('usage: hackios_tool.py get | fetch | assemble | boot [-- OPTIONS] | run [-- OPTIONS] | wifi NAME PASS | android | status | tools')
    command, rest = argv[0].lstrip('-'), argv[1:]
    options = []
    if '--' in rest:
        index = rest.index('--')
        rest, options = rest[:index], rest[index + 1:]
    if command == 'get':
        started = time.time()
        step(1, 3, 'the release kit')
        get_kit()
        step(2, 3, 'iOS 16.5 from Apple (about 7 GB the first time; already fetched parts are skipped)')
        fetch_ios()
        step(3, 3, 'the root and system images')
        assemble()
        minutes = int((time.time() - started) // 60)
        print(f'\n{GREEN}hackios: ready in {minutes} min. Now: --boot{RESET}', flush=True)
    elif command == 'fetch':
        fetch_ios()
    elif command == 'assemble':
        assemble()
    elif command == 'boot':
        boot_uefi()
        run_ios(options)
    elif command == 'run':
        run_ios(options)
    elif command == 'wifi':
        wifi(rest)
    elif command == 'android':
        android()
    elif command == 'status':
        status()
    elif command == 'tools':
        get_platform_tools()
        say('adb and fastboot are in work/platform-tools')
    else:
        die('unknown command ' + command)


def guarded(argv):
    try:
        main(argv)
    except KeyboardInterrupt:
        print(flush=True)
        die('stopped (Ctrl+C). Run the same command again to continue.')
    except Exception:
        import traceback
        LOG.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG, 'a', encoding='utf-8') as log:
            log.write(time.strftime('\n%Y-%m-%d %H:%M:%S ') + ' '.join(argv) + '\n')
            traceback.print_exc(file=log)
        error = sys.exc_info()[1]
        die(f'unexpected error: {type(error).__name__}: {error}\n'
            f'hackios: details in {LOG.relative_to(ROOT)}; send that file when you report the problem')


if __name__ == '__main__':
    guarded(sys.argv[1:])
