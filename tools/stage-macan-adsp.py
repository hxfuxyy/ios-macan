#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import shutil
import stat
import struct
import tempfile
import time
from macan_adsp_firmware import check
from mkpackfs import SUPER, DNODE, MAGIC, SUPER_SIZE, align

ROOT = Path(__file__).resolve().parents[1]

def stage_image(image, sources):
    with image.open('rb') as f:
        super_raw = bytearray(f.read(SUPER_SIZE))
        magic, block, count, nodes_off, strings_off, strings_size, data_off, image_size, created = SUPER.unpack_from(super_raw)
        assert magic == MAGIC and image_size == image.stat().st_size
        assert 4096 <= block <= 65536 and block & (block - 1) == 0
        f.seek(strings_off)
        strings = f.read(strings_size)
        f.seek(nodes_off)
        nodes = []
        for _ in range(count):
            fields = list(DNODE.unpack(f.read(DNODE.size)))
            name = strings[fields[7]:fields[7] + fields[8]]
            target = strings[fields[5]:fields[5] + fields[4]] if stat.S_ISLNK(fields[0]) else None
            nodes.append(dict(fields=fields, name=name, target=target, children={}))
    for i, node in enumerate(nodes[1:], 1):
        parent = node['fields'][6]
        assert 0 <= parent < count and stat.S_ISDIR(nodes[parent]['fields'][0])
        nodes[parent]['children'][node['name']] = node
    existing_fw = nodes[0]['children'].get(b'fw')
    if existing_fw and all(source.name.encode() in existing_fw['children'] for source in sources):
        with image.open('rb') as f:
            same = True
            for source in sources:
                fields = existing_fw['children'][source.name.encode()]['fields']
                if fields[11] or fields[4] != source.stat().st_size:
                    same = False
                    break
                f.seek(fields[5])
                if f.read(fields[4]) != source.read_bytes():
                    same = False
                    break
        if same:
            print(f'{image}: ADSP /fw already matches ({sum(p.stat().st_size for p in sources)} bytes)')
            return
    now = int(time.time())
    def new(name, mode):
        return dict(fields=[mode, 0, 0, 1, 0, 0, 0, 0, len(name), 0, now, 0],
                    name=name, target=None, children={})
    root = nodes[0]
    fw = root['children'].get(b'fw')
    if fw is None:
        fw = new(b'fw', stat.S_IFDIR | 0o755)
        root['children'][b'fw'] = fw
    assert stat.S_ISDIR(fw['fields'][0])
    for source in sources:
        node = new(source.name.encode(), stat.S_IFREG | 0o644)
        node['source'] = source
        fw['children'][node['name']] = node
    ordered = [root]
    root['parent'] = root
    for node in ordered:
        node['index'] = 0 if node is root else node['index']
        children = [node['children'][name] for name in sorted(node['children'])]
        for child in children:
            child['parent'] = node
            child['index'] = len(ordered)
            ordered.append(child)
    strings = bytearray(b'\0')
    for node in ordered:
        fields = node['fields']
        fields[6] = node['parent']['index']
        fields[7] = len(strings)
        strings += node['name'] + b'\0'
        if node['target'] is not None:
            fields[5] = len(strings)
            strings += node['target'] + b'\0'
        if stat.S_ISDIR(fields[0]):
            fields[4] = len(node['children'])
            fields[5] = min((c['index'] for c in node['children'].values()), default=0)
            fields[3] = 2 + sum(stat.S_ISDIR(c['fields'][0]) for c in node['children'].values())
    fd, temp = tempfile.mkstemp(prefix=image.name + '.adsp-', dir=image.parent)
    try:
        with os.fdopen(fd, 'w+b') as out, image.open('rb') as old:
            shutil.copyfileobj(old, out, 8 * 1024 * 1024)
            for node in ordered:
                if 'source' not in node:
                    continue
                out.seek(align(out.tell(), block))
                node['fields'][5] = out.tell()
                node['fields'][4] = node['source'].stat().st_size
                with node['source'].open('rb') as source:
                    shutil.copyfileobj(source, out)
            new_nodes = align(out.tell(), 4096)
            out.seek(new_nodes)
            for node in ordered:
                out.write(DNODE.pack(*node['fields']))
            new_strings = align(out.tell(), 4096)
            out.seek(new_strings)
            out.write(strings)
            end = align(out.tell(), block)
            out.truncate(end)
            SUPER.pack_into(super_raw, 0, magic, block, len(ordered), new_nodes, new_strings,
                            len(strings), data_off, end, now)
            out.seek(0)
            out.write(super_raw)
            out.flush()
            os.fsync(out.fileno())
        os.chmod(temp, image.stat().st_mode & 0o777)
        with open(temp, 'rb') as f:
            for node in ordered:
                if 'source' in node:
                    f.seek(node['fields'][5])
                    assert f.read(node['fields'][4]) == node['source'].read_bytes()
        os.replace(temp, image)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)
    print(f'{image}: {image_size} -> {end} bytes; /fw: {len(sources)} ADSP files, raw {sum(p.stat().st_size for p in sources)} bytes')

def main():
    parser = argparse.ArgumentParser(description='')
    parser.add_argument('--firmware', type=Path, default=ROOT / 'artifacts/ace6t/hw-android/adsp')
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--directory', type=Path, help='System/flashboot staging directory; creates fw/')
    group.add_argument('--system', type=Path, help='Existing system.packfs, replaced atomically')
    args = parser.parse_args()
    check(args.firmware)
    sources = sorted(set(args.firmware.glob('adsp*.mdt')) | set(args.firmware.glob('adsp*.b[0-9][0-9]')))
    assert sources
    if args.directory:
        dest = args.directory / 'fw'
        dest.mkdir(parents=True, exist_ok=True)
        for source in sources:
            shutil.copyfile(source, dest / source.name)
            (dest / source.name).chmod(0o644)
        print(f'{dest}: {len(sources)} files, {sum(p.stat().st_size for p in sources)} bytes')
    else:
        stage_image(args.system, sources)

if __name__ == '__main__':
    main()
