#!/usr/bin/env python3

import importlib.util
import os
import stat
import sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('select_system', os.path.join(here, 'select-system.py'))
select_system = importlib.util.module_from_spec(spec)
spec.loader.exec_module(select_system)

SKIP = ('private/var/staged_system_apps', 'private/var/MobileAsset/')
EXTRA = [
    ('/private/var/db/uuidtext', 0o755, 272, 272),
    ('/private/var/db/diagnostics/timesync', 0o755, 272, 272),
    ('/private/var/containers', 0o755, 0, 0),
    ('/private/var/containers/Data', 0o755, 0, 0),
    ('/private/var/containers/Shared', 0o755, 0, 0),
    ('/private/var/containers/Bundle', 0o755, 0, 0),
    ('/private/var/mobile/Containers', 0o755, 501, 501),
    ('/private/var/mobile/Containers/Data', 0o755, 501, 501),
    ('/private/var/mobile/Containers/Shared', 0o755, 501, 501),
    ('/private/var/mobile/Library/Caches', 0o755, 501, 501),
    ('/private/var/mobile/Library/Preferences', 0o755, 501, 501),
    ('/private/var/mobile/Library/Logs', 0o755, 501, 501),
    ('/private/var/root/Library/Caches', 0o700, 0, 0),
    ('/private/var/root/Library/Preferences', 0o700, 0, 0),
    ('/private/var/root/Library/Logs', 0o700, 0, 0),
]

dirs, links = {}, []
for e in select_system.entries(sys.argv[1]):
    path = e['Path']
    if not path.startswith('private/var') or path.startswith(SKIP):
        continue
    mode = select_system.mode_bits(e['Mode'])
    if mode is None:
        continue
    if stat.S_ISDIR(mode):
        dirs['/' + path] = (mode & 0o7777, int(e.get('User ID') or 0), int(e.get('Group ID') or 0))
    elif stat.S_ISLNK(mode):
        links.append((e.get('Symbolic Link', ''), '/' + path))
for path, mode, uid, gid in EXTRA:
    dirs.setdefault(path, (mode, uid, gid))
dirs.pop('/private/var', None)
for path in sorted(dirs):
    mode, uid, gid = dirs[path]
    print(f'd\t{mode:o}\t{uid}\t{gid}\t{path}')
for target, path in links:
    print(f'l\t{target}\t{path}')
print('l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo')
