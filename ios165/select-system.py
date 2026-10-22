#!/usr/bin/env python3

import re
import stat
import sys

WHOLE = ('usr/libexec/', 'usr/sbin/', 'usr/bin/', 'usr/lib/', 'usr/share/', 'sbin/', 'bin/', 'private/etc/',
         'System/Library/', 'Applications/', 'Library/')
SKIP_SYSTEM_LIBRARY = ('LinguisticData', 'PreinstalledAssetsV2', 'NanoTimeKit', 'Audio',
                       'Assistant', 'AccessibilityBundles',
                       'VideoProcessors', 'ScreenReader', 'Photos', 'Carrier Bundles', 'NanoPreferenceBundles',
                       'ImagingNetworks', 'ProductDocuments', 'AppPlaceholders', 'UserNotifications',
                       'Caches')
FRAMEWORKS = ('System/Library/Frameworks/', 'System/Library/PrivateFrameworks/')
SMALL = 4 << 20
SKIP_EXT = ('.weights', '.weights_nonane', '.metallib', '.mov', '.pcm', '.scnz', '.espresso.net',
            '.espresso.shape', '.caf', '.m4a', '.aiff', '.mp4', '.heic', '.wav', '.art', '.scube',
            '.ds', '.dat', '.bin')
DATA_FRAMEWORKS = tuple(f'System/Library/{name}.framework/' for name in (
    'PrivateFrameworks/TextInputUI', 'PrivateFrameworks/CoreEmoji', 'Frameworks/CFNetwork'))
SKIP_APP_EXT = ('.mov', '.mp4', '.m4a', '.caf', '.wav', '.heic', '.aiff', '.metallib', '.scnz', '.usdz')
MAX_ASSET_CATALOG = 4 << 20
MAX_FONT = 25 << 20
TRIMMED_FONTS = ('System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc',
                 'System/Library/Fonts/LanguageSupport/PingFang.ttc')
MAX_STRING_TABLE = 16 << 20
STAGED_APPS = 'private/var/staged_system_apps/'
ALL_PICTURES = ('System/Library/PrivateFrameworks/ProgressUI.framework/',)
BIG_CATALOGS = ('System/Library/PrivateFrameworks/SFSymbols.framework/',)
HEALTH = 'System/Library/Health/'
SKIP_PREFIX = ('usr/share/tokenizer/', 'usr/share/firmware/', 'usr/share/mecabra/',
               'usr/standalone/', 'Library/Audio/', 'Library/Ringtones/',
               HEALTH + 'Assets/FitnessUIAssets.bundle/') + \
    tuple(f'System/Library/{name}/' for name in SKIP_SYSTEM_LIBRARY)
SERVICES = ('usr/sbin/fairplayd.H2', 'usr/libexec/lskdd', 'usr/libexec/searchpartyd',
            'System/Library/PrivateFrameworks/IDS.framework/identityservicesd.app/',
            'System/Library/PrivateFrameworks/PassKitCore.framework/passd',
            'System/Library/PrivateFrameworks/AssistantServices.framework/assistantd',
            'System/Library/PrivateFrameworks/TelephonyUtilities.framework/callservicesd')
BIG_PROGRAM = 10 << 20
AUDIO = ('Library/Audio/Plug-Ins/', 'Library/Audio/Tunings/D22/', 'Library/Audio/Tunings/Generic/',
         'System/Library/Audio/UISounds/')
SKIP_AUDIO = ('System/Library/Audio/UISounds/nano/',)
KEEP_LANG = ('en', 'Base', 'English', 'en_US', 'en_GB')

def entries(listing):

    entry = {}
    for line in open(listing, errors='replace'):
        line = line.rstrip('\n')
        if not line:
            if 'Path' in entry and 'Mode' in entry:
                yield entry
            entry = {}
        elif ' = ' in line or line.endswith(' ='):
            key, _, value = line.partition(' =')
            entry[key] = value[1:] if value.startswith(' ') else value
    if 'Path' in entry and 'Mode' in entry:
        yield entry

def mode_bits(text):

    kinds = {'-': stat.S_IFREG, 'd': stat.S_IFDIR, 'l': stat.S_IFLNK}
    if text[0] not in kinds:
        return None
    mode = kinds[text[0]]
    for i, ch in enumerate(text[1:10]):
        if ch in 'rwxst':
            mode |= 0o400 >> i
        if ch in 'sS' and i == 2:
            mode |= stat.S_ISUID
        if ch in 'sS' and i == 5:
            mode |= stat.S_ISGID
        if ch in 'tT' and i == 8:
            mode |= stat.S_ISVTX
        if ch in 'ST':
            mode &= ~(0o400 >> i)
    return mode

def wanted_file(path, size):
    lproj = re.search(r'/([^/]+)\.lproj/', path)
    if lproj and lproj.group(1) not in KEEP_LANG:
        return False
    if path.startswith(SERVICES):
        return True
    if path.startswith(AUDIO):
        return not path.startswith(SKIP_AUDIO) and '.espresso.' not in path
    if path.startswith(SKIP_PREFIX):
        return False
    if path.startswith('System/Library/Fonts/'):
        return size <= MAX_FONT or path in TRIMMED_FONTS
    if path.startswith(ALL_PICTURES) and path.lower().endswith('.png'):
        return True
    if path.startswith(('usr/libexec/', 'usr/sbin/')):
        return size <= BIG_PROGRAM
    if path.startswith(('Applications/', HEALTH)):
        return not path.lower().endswith(SKIP_APP_EXT) and '.mlmodelc/' not in path
    if path.startswith(FRAMEWORKS):
        if re.search(r'/(XPCServices|Support|Helpers)/', path):
            return size <= 32 << 20 and not path.lower().endswith(('.png', '.car', '.loctable', '.caml'))
        if path.endswith('.car'):
            return size <= MAX_ASSET_CATALOG or path.startswith(BIG_CATALOGS)
        if path.endswith('.loctable'):
            return size <= MAX_STRING_TABLE
        if path.startswith(DATA_FRAMEWORKS) and path.lower().endswith(('.dat', '.bin')):
            return size <= SMALL
        return size <= SMALL and not path.lower().endswith(SKIP_EXT) and '.mlmodelc/' not in path
    return path.startswith(WHOLE)

def main():
    listing, files_out, manifest_out = sys.argv[1:4]
    meta = {}
    source = {}
    for e in entries(listing):
        mode = mode_bits(e['Mode'])
        path = e['Path']
        if mode is None:
            continue
        if path.startswith(STAGED_APPS):
            source['Applications/' + path[len(STAGED_APPS):]] = path
            path = 'Applications/' + path[len(STAGED_APPS):]
        elif path.startswith('private/var/'):
            continue
        meta[path] = (mode, int(e.get('User ID') or 0), int(e.get('Group ID') or 0),
                      e.get('Symbolic Link', ''), int(e.get('Size') or 0))

    chosen = set()
    for path, (mode, uid, gid, target, size) in meta.items():
        if stat.S_ISREG(mode):
            keep = wanted_file(path, size)
        elif stat.S_ISLNK(mode):
            keep = '/' not in path or path.startswith(WHOLE) and not path.startswith(SKIP_PREFIX)
        else:
            lproj = re.search(r'/([^/]+)\.lproj$', path)
            keep = '/' not in path or path in ('private/var', 'private/tmp', 'private/etc', 'private/preboot',
                                               'private/xarts', 'private') or \
                bool(lproj) and lproj.group(1) in KEEP_LANG and \
                (path.startswith(WHOLE) and not path.startswith(SKIP_PREFIX) or
                 path.startswith(AUDIO) and not path.startswith(SKIP_AUDIO))
        if keep:
            chosen.add(path)
    for path in list(chosen):
        while '/' in path:
            path = path.rsplit('/', 1)[0]
            chosen.add(path)

    total = count = 0
    with open(files_out, 'w') as files, open(manifest_out, 'w') as manifest:
        for path in sorted(chosen):
            if path not in meta:
                continue
            mode, uid, gid, target, size = meta[path]
            manifest.write(f'{mode:o}\t{uid}\t{gid}\t{path}\t{target if stat.S_ISLNK(mode) else ""}\n')
            if stat.S_ISREG(mode):
                files.write(source.get(path, path) + '\n')
                total += size
                count += 1
    print(f'select-system: {count} files, {total / 1048576:.0f} MB; {len(chosen)} entries in the manifest')

if __name__ == '__main__':
    main()
