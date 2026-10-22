#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
A = ROOT / 'artifacts' / 'ace6t'
KIT = ROOT / 'work' / 'kit'
IMAGES = ROOT / 'work' / 'images'
RELEASE = 'https://github.com/hxfuxyy/ios-macan/releases/latest/download/hackios-kit.zip'
HW = A / 'hw-android'
WINDOWS = os.name == 'nt'


def say(text):
    print('hackios: ' + text, flush=True)


def die(text):
    print('hackios: ' + text, file=sys.stderr, flush=True)
    sys.exit(1)


def find_tool(name):
    exe = name + ('.exe' if WINDOWS else '')
    found = shutil.which(name)
    if found:
        return found
    local = ROOT / 'work' / 'platform-tools' / exe
    if local.exists():
        return str(local)
    die(f'{name} not found: install Android platform-tools (hackios.bat --tools on Windows) or put {exe} in PATH')


ADB = FASTBOOT = None


def tools():
    global ADB, FASTBOOT
    if not ADB:
        ADB, FASTBOOT = find_tool('adb'), find_tool('fastboot')


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


def adb_root(command, timeout=60):
    _, out = call([ADB, 'shell', 'su', '-c', command], timeout)
    return out.replace('\r', '')


def wait_for(seconds, what, check):
    deadline = time.time() + seconds
    while not check():
        if time.time() > deadline:
            die(f'timeout after {seconds} s waiting for {what}')
        time.sleep(2)


def boot_uefi():
    tools()
    path = KIT / 'uefi.img'
    if not path.is_file():
        die('no UEFI image: run --get (or --build)')
    if not fastboot_devices() and not adb_ready():
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
        if not adb_root('id', 10).startswith('uid=0'):
            die('Android has no working root (su): it cannot be sent to the bootloader')
        say('Android is up: rebooting into the bootloader')
        call([ADB, 'shell', 'su', '-c', 'reboot bootloader'], 15)
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
    sys.exit(subprocess.call(command + (options or ['--seconds', '0'])))


def get_kit():
    import urllib.request
    import zipfile
    KIT.mkdir(parents=True, exist_ok=True)
    archive = ROOT / 'work' / 'hackios-kit.zip'
    say('downloading the release kit (kernel, UEFI, userland)')
    with urllib.request.urlopen(RELEASE, timeout=120) as response, open(archive, 'wb') as out:
        shutil.copyfileobj(response, out)
    with zipfile.ZipFile(archive) as kit:
        kit.extractall(KIT)
    archive.unlink()
    if not WINDOWS:
        for name in ('unzbm',):
            path = KIT / 'tools' / name
            if path.exists():
                path.chmod(0o755)
    say('kit ready in work/kit: ' + ', '.join(sorted(p.name for p in KIT.iterdir())))


def fetch_ios():
    subprocess.run([sys.executable, str(ROOT / 'ios165' / 'fetch.py')], check=True)


def assemble():
    subprocess.run([sys.executable, str(ROOT / 'tools' / 'assemble.py')], check=True)


def pull_named(name, dest):
    where = adb_root('for d in /vendor/firmware_mnt/image /vendor/firmware /odm/firmware /vendor/firmware/wcn7750 /odm/etc; '
                     f'do [ -f $d/{name} ] && echo $d/{name} && break; done').strip()
    if not where:
        where = adb_root(f"find /vendor /odm -name '{name}' 2>/dev/null | head -1").strip()
    if not where:
        say(f'not on the phone: {name}')
        return
    adb_root(f"cp '{where}' /data/local/tmp/hackios.fw && chmod 644 /data/local/tmp/hackios.fw")
    dest.mkdir(parents=True, exist_ok=True)
    call([ADB, 'pull', '/data/local/tmp/hackios.fw', str(dest / name)], 120)


def firmware():
    tools()
    if not adb_ready():
        die('the phone is not in Android over adb (USB debugging on?)')
    if not adb_root('id', 10).startswith('uid=0'):
        die('no root (su) on the phone')
    say('GPU')
    for name in ('gen80900_aqe.fw', 'gen80900_gmu.bin', 'gen80900_sqe.fw', 'gen80900_zap.mbn'):
        pull_named(name, A / 'gpu-android')
    say('Wi-Fi')
    names = ['wpss.mdt'] + [f'wpss.b{n:02d}' for n in range(13)] + [
        'aux_ucode.elf', 'bd_m20gl.elf', 'phy_ucode.elf', 'regdb_xiaomi.bin', 'qdss_trace_config_perf_v1.cfg']
    for name in names:
        pull_named(name, A / 'wifi-android' / 'fw')
    say('vibration')
    pull_named('aw8697_haptic_170.bin', HW / 'fw')
    say('audio DSP')
    for name in adb_root('ls /vendor/firmware_mnt/image').split():
        if re.fullmatch(r'adsp(_dtb)?\.(mdt|b\d+)', name):
            pull_named(name, HW / 'adsp')
    adb_root('rm -f /data/local/tmp/hackios.fw')
    say('done: the firmware is in artifacts/ace6t (it never goes into git)')


def wifi(ssid, password):
    path = A / 'wifi-android' / 'fw' / 'network'
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f'{ssid}\n{password}\n')
    if not WINDOWS:
        path.chmod(0o600)
    say('network saved: rebuild the images with --build')


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
    for name in ('gpu-android', 'wifi-android/fw', 'hw-android/fw', 'hw-android/adsp'):
        folder = A / name
        count = len(list(folder.iterdir())) if folder.is_dir() else 0
        print(f'firmware {name + ":":17}', count, 'files')


def main(argv):
    if not argv:
        die('usage: hackios_tool.py get | fetch | firmware | assemble | boot [-- OPTIONS] | run [-- OPTIONS] | wifi NAME PASS | android | status')
    command, rest = argv[0].lstrip('-'), argv[1:]
    options = []
    if '--' in rest:
        index = rest.index('--')
        rest, options = rest[:index], rest[index + 1:]
    if command == 'get':
        get_kit()
        fetch_ios()
        tools()
        if adb_ready():
            firmware()
        else:
            say('no phone in Android over adb: the firmware is copied later (--firmware, then --assemble)')
        assemble()
        say('ready: now --boot')
    elif command == 'fetch':
        fetch_ios()
    elif command == 'assemble':
        assemble()
    elif command == 'boot':
        boot_uefi()
        run_ios(options)
    elif command == 'run':
        run_ios(options)
    elif command == 'firmware':
        firmware()
    elif command == 'wifi':
        if len(rest) != 2:
            die('usage: --wifi NAME PASS')
        wifi(*rest)
    elif command == 'android':
        android()
    elif command == 'status':
        status()
    else:
        die('unknown command ' + command)


if __name__ == '__main__':
    main(sys.argv[1:])
