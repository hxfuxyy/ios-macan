#!/usr/bin/env python3

import os
import plistlib
import sys

DENY = ('watchdog', 'CommCenter', 'wifi', 'WiFi', 'bluetooth', 'BTServer', 'locationd', 'softwareupdate',
        'datamigrator', 'backupd', 'thermalmonitord', 'keybagd', 'nfcd', 'biometrickitd', 'pearld',
        'obliterat', 'storage_mounter', 'com.apple.fud', 'sepd', 'seputil', 'camera', 'ReportCrash',
        'osanalytics', 'lockdown', 'audio', 'jetsamproperties',
        'purplebuddy', 'restore', 'usb', 'accessoryd',
        'mobileassetd', 'bird',
        'homed', 'routined', 'duetexpertd', 'com.apple.dmd', 'syncdefaultsd', 'photoanalysisd', 'avconferenced')

DENY = tuple(word for word in DENY if word not in os.environ.get('ALLOW', '').split())

args = sys.argv[1:]
rest = '--rest' in args
root_volume = None
if '--root-volume' in args:
    i = args.index('--root-volume')
    root_volume = args[i + 1]
    del args[i:i + 2]
args = [a for a in args if a != '--rest']
src_root, dest, labels = args[0], args[1], args[2:]
oslog = os.environ.get('OSLOG', '').split()
src = f'{src_root}/System/Library/LaunchDaemons'
CRYPTEXES = {'/System/Cryptexes/OS/': 'cryptex', '/System/Cryptexes/App/': 'cryptex-app'}

def volume_paths(listing, _cache={}):

    if listing not in _cache:
        _cache[listing] = {line[len('Path = '):].strip()
                           for line in open(listing, errors='replace') if line.startswith('Path = ')}
    return _cache[listing]

def present(program):
    for prefix, unpacked in CRYPTEXES.items():
        if program.startswith(prefix):
            return os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(src_root)), unpacked,
                                               program[len(prefix):]))
    if root_volume:
        return program.lstrip('/') in volume_paths(root_volume)
    return os.path.isfile(f'{src_root}{program}')

angels = f'{src_root}/System/Library/LaunchAngels'
os.makedirs(dest, exist_ok=True)
for label in labels:
    with open(f'{src}/{label}.plist', 'rb') as f:
        job = plistlib.load(f)
    job['StandardErrorPath'] = '/dev/console'
    job.pop('_LimitLoadToDeviceTree', None)
    if 'all' in oslog or label in oslog:
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    with open(f'{dest}/{label}.plist', 'wb') as f:
        plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
    program = job.get('Program') or job['ProgramArguments'][0]
    missing = '' if present(program) else ('   (NOT on the iOS volume!)' if root_volume
                                           else '   (NOT in the system image!)')
    print(f'  job {label}: {program}{missing}')

if rest:
    count = 0
    rest_jobs = [(src, name) for name in sorted(os.listdir(src))]
    if os.path.isdir(angels):
        rest_jobs += [(angels, name) for name in sorted(os.listdir(angels))]
    for directory, name in rest_jobs:
        label = name[:-len('.plist')]
        if not name.endswith('.plist') or label in labels or any(word in label for word in DENY):
            continue
        with open(f'{directory}/{name}', 'rb') as f:
            job = plistlib.load(f)
        program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
        if not job.get('MachServices') or not present(program):
            continue
        for key in ('RunAtLoad', 'KeepAlive', 'StartInterval', 'StartCalendarInterval', 'WatchPaths',
                    'QueueDirectories', 'StartOnMount', 'LaunchEvents'):
            job.pop(key, None)
        job['StandardErrorPath'] = '/dev/console'
        if label in oslog:
            job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
        with open(f'{dest}/{name}', 'wb') as f:
            plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
        count += 1
    print(f'  ... and {count} more jobs, started on demand only')
