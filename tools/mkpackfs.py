#!/usr/bin/env python3

import argparse
import concurrent.futures
import ctypes
import ctypes.util
import os
import stat
import struct
import sys
import time

MAGIC = b'PACKFS1\0'
SUPER_SIZE = 4096
META_ALIGN = 4096
SUPER = struct.Struct('<8sIIQQQQQQ')
DNODE = struct.Struct('<IIIIQQIIIIQQ')
MAX_NAME = 255
F_CHUNKED = 0x1
F_ZSTD = 0x2
CHUNK = 65536

def _library(name, soname):
    for candidate in (ctypes.util.find_library(name), soname):
        if candidate:
            try:
                return ctypes.CDLL(candidate)
            except OSError:
                pass
    return None

_lz4 = None

def lz4_block(data, level):

    global _lz4
    if _lz4 is None:
        _lz4 = _library('lz4', 'liblz4.so.1') or False
    if _lz4 is False:
        import lz4.block
        return lz4.block.compress(data, mode='high_compression' if level > 0 else 'default',
                                  compression=max(level, 0), store_size=False)
    bound = _lz4.LZ4_compressBound(len(data))
    out = ctypes.create_string_buffer(bound)
    if level > 0:
        n = _lz4.LZ4_compress_HC(data, out, len(data), bound, level)
    else:
        n = _lz4.LZ4_compress_default(data, out, len(data), bound)
    if n <= 0:
        raise RuntimeError('LZ4 failed')
    return out.raw[:n]

_zstd = None

def zstd_frame(data, level):

    global _zstd
    if _zstd is None:
        _zstd = _library('zstd', 'libzstd.so.1')
        if _zstd is None:
            import zstandard
            _zstd = zstandard
        else:
            for name in ('ZSTD_compressBound', 'ZSTD_compress'):
                getattr(_zstd, name).restype = ctypes.c_size_t
            _zstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
            _zstd.ZSTD_compress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
    if not isinstance(_zstd, ctypes.CDLL):
        return _zstd.ZstdCompressor(level=level).compress(data)
    bound = _zstd.ZSTD_compressBound(len(data))
    out = ctypes.create_string_buffer(bound)
    n = _zstd.ZSTD_compress(out, bound, data, len(data), level)
    if _zstd.ZSTD_isError(ctypes.c_size_t(n)):
        raise RuntimeError('Zstandard failed')
    return out.raw[:n]

def chunked_extent(job):

    path, level, zstd, raw_file = job
    with open(long_path(path), 'rb') as f:
        data = f.read()
    if raw_file:
        return len(data), data
    chunks = []
    for at in range(0, len(data), CHUNK):
        raw = data[at:at + CHUNK]
        packed = zstd_frame(raw, level) if zstd else lz4_block(raw, level)
        chunks.append(packed if len(packed) < len(raw) else raw)
    table = [(len(chunks) + 1) * 4]
    for chunk in chunks:
        table.append(table[-1] + len(chunk))
    if table[-1] >= 1 << 32:
        raise RuntimeError(f'{path}: too big for a chunked file')
    return len(data), struct.pack(f'<{len(table)}I', *table) + b''.join(chunks)

def align(value, to):
    return (value + to - 1) // to * to

class Node:
    def __init__(self, name, mode, mtime, source=None, target=None):
        self.name = name
        self.mode = mode
        self.uid = self.gid = 0
        self.mtime = int(mtime)
        self.source = source
        self.target = target
        self.children = {}
        self.parent = None
        self.index = None
        self.data = self.size = 0
        self.name_off = 0

def scan(root):

    skipped = []

    def walk(path, name):
        st = os.lstat(long_path(path))
        if stat.S_ISDIR(st.st_mode):
            node = Node(name, st.st_mode, st.st_mtime)
            for entry in os.listdir(long_path(path)):
                child = walk(os.path.join(path, entry), os.fsencode(entry))
                if child is not None:
                    node.children[child.name] = child
            return node
        if stat.S_ISREG(st.st_mode):
            return Node(name, st.st_mode, st.st_mtime, source=path)
        if stat.S_ISLNK(st.st_mode):
            return Node(name, st.st_mode, st.st_mtime, target=os.fsencode(os.readlink(long_path(path))))
        skipped.append(path)
        return None

    return walk(root, b''), skipped

def apply_manifest(root, manifest):

    added = 0
    for line in open(manifest, 'rb'):
        fields = line.rstrip(b'\n').split(b'\t')
        if len(fields) < 4:
            continue
        mode, uid, gid, path = int(fields[0], 8), int(fields[1]), int(fields[2]), fields[3]
        target = fields[4] if len(fields) > 4 else b''
        node = root
        parts = [p for p in path.split(b'/') if p]
        for depth, part in enumerate(parts):
            child = node.children.get(part)
            last = depth == len(parts) - 1
            if child is None:
                if not last or stat.S_ISREG(mode) or not stat.S_ISDIR(node.mode):
                    node = None
                    break
                if stat.S_ISLNK(mode):
                    child = Node(part, mode, root.mtime, target=target)
                elif stat.S_ISDIR(mode):
                    child = Node(part, mode, root.mtime)
                else:
                    node = None
                    break
                node.children[part] = child
                added += 1
            node = child
        if node is not None and stat.S_ISLNK(mode) and stat.S_ISREG(node.mode):
            node.mode, node.uid, node.gid, node.source, node.target = mode, uid, gid, None, target
        elif node is not None and stat.S_IFMT(node.mode) == stat.S_IFMT(mode):
            node.mode, node.uid, node.gid = mode, uid, gid
    return added

def layout(root):

    nodes = [root]
    root.index = 0
    root.parent = root
    queue = [root]
    while queue:
        directory = queue.pop(0)
        for name in sorted(directory.children):
            child = directory.children[name]
            if len(name) > MAX_NAME:
                sys.exit(f'mkpackfs: name too long: {name!r}')
            child.parent = directory
            child.index = len(nodes)
            nodes.append(child)
            if stat.S_ISDIR(child.mode):
                queue.append(child)
    return nodes

def build_chunked(root, block, output, level, zstd=False, raw_sources=()):

    nodes = layout(root)
    strings = bytearray(b'\0')
    for node in nodes:
        if node.index:
            node.name_off = len(strings)
            strings += node.name + b'\0'
    nodes_off = SUPER_SIZE
    strings_off = align(nodes_off + len(nodes) * DNODE.size, META_ALIGN)
    for node in nodes:
        if stat.S_ISLNK(node.mode):
            node.data, node.size = len(strings), len(node.target)
            strings += node.target + b'\0'
        elif stat.S_ISDIR(node.mode):
            ordered = sorted(node.children)
            node.data = node.children[ordered[0]].index if ordered else 0
            node.size = len(ordered)
    data_off = align(strings_off + len(strings), META_ALIGN)
    files = [n for n in nodes if stat.S_ISREG(n.mode)]
    raw_total = 0

    with open(output, 'wb') as out, concurrent.futures.ProcessPoolExecutor() as pool:
        out.seek(data_off)
        pending = []
        todo = iter(files)

        def submit():
            node = next(todo, None)
            if node is not None:
                pending.append((node, pool.submit(chunked_extent, (node.source, level, zstd, node.source in raw_sources))))

        for _ in range(2 * (os.cpu_count() or 2)):
            submit()
        while pending:
            node, future = pending.pop(0)
            size, extent = future.result()
            submit()
            node.size = size
            raw_total += size
            if size:
                alignment = block if node.source in raw_sources else 8
                out.write(b'\0' * (-out.tell() % alignment))
                node.data = out.tell()
                out.write(extent)
        image_size = align(out.tell(), block)
        out.write(b'\0' * (image_size - out.tell()))

        meta = bytearray(data_off)
        SUPER.pack_into(meta, 0, MAGIC, block, len(nodes), nodes_off, strings_off, len(strings), data_off, image_size,
                        max(n.mtime for n in nodes))
        for node in nodes:
            if stat.S_ISDIR(node.mode):
                nlink = 2 + sum(stat.S_ISDIR(c.mode) for c in node.children.values())
            else:
                nlink = 1
            flags = (F_CHUNKED | (F_ZSTD if zstd else 0)) if stat.S_ISREG(node.mode) and node.size and node.source not in raw_sources else 0
            DNODE.pack_into(meta, nodes_off + node.index * DNODE.size, node.mode, node.uid, node.gid, nlink, node.size,
                            node.data, node.parent.index, node.name_off, len(node.name), 0, node.mtime, flags)
        meta[strings_off:strings_off + len(strings)] = strings
        out.seek(0)
        out.write(meta)
    print(f'{output}: {raw_total // 1048576} MB of files in {(image_size - data_off) // 1048576} MB', file=sys.stderr)
    return image_size, len(nodes)

def build(root, block, output, page=None):

    page = page or block
    nodes = layout(root)

    strings = bytearray(b'\0')
    for node in nodes:
        if node.index:
            node.name_off = len(strings)
            strings += node.name + b'\0'

    nodes_off = SUPER_SIZE
    strings_off = align(nodes_off + len(nodes) * DNODE.size, META_ALIGN)

    for node in nodes:
        if stat.S_ISLNK(node.mode):
            node.data, node.size = len(strings), len(node.target)
            strings += node.target + b'\0'
        elif stat.S_ISDIR(node.mode):
            ordered = sorted(node.children)
            node.data = node.children[ordered[0]].index if ordered else 0
            node.size = len(ordered)

    data_off = align(strings_off + len(strings), page)
    cursor = data_off
    files = [n for n in nodes if stat.S_ISREG(n.mode)]
    for node in files:
        node.size = os.path.getsize(long_path(node.source))
    for node in sorted(files, key=lambda n: (n.size < page, n.index)):
        if node.size:
            cursor = align(cursor, page if node.size >= page else block)
            node.data = cursor
            cursor += align(node.size, block)
    image_size = align(cursor, page)

    meta = bytearray(data_off)
    SUPER.pack_into(meta, 0, MAGIC, block, len(nodes), nodes_off, strings_off, len(strings), data_off, image_size,
                    max(n.mtime for n in nodes))
    for node in nodes:
        if stat.S_ISDIR(node.mode):
            nlink = 2 + sum(stat.S_ISDIR(c.mode) for c in node.children.values())
        else:
            nlink = 1
        DNODE.pack_into(meta, nodes_off + node.index * DNODE.size, node.mode, node.uid, node.gid, nlink, node.size,
                        node.data, node.parent.index, node.name_off, len(node.name), 0, node.mtime, 0)
    meta[strings_off:strings_off + len(strings)] = strings

    with open(output, 'wb') as out:
        out.write(meta)
        for node in sorted((n for n in files if n.size), key=lambda n: n.data):
            out.write(b'\0' * (node.data - out.tell()))
            copied = 0
            with open(long_path(node.source), 'rb') as f:
                while chunk := f.read(8 << 20):
                    out.write(chunk)
                    copied += len(chunk)
            if copied != node.size:
                sys.exit(f'mkpackfs: {node.source} changed while being packed')
            out.write(b'\0' * (align(copied, block) - copied))
        out.write(b'\0' * (image_size - out.tell()))
    return image_size, len(nodes)


def long_path(path):
    if os.name != 'nt' or not isinstance(path, str) or path.startswith('\\\\?\\'):
        return path
    return '\\\\?\\' + os.path.abspath(path)

def _parts(path):
    return [os.fsencode(p) for p in str(path).replace('\\', '/').split('/') if p and p != '.']

def node_at(root, path, create=False, mode=0o40755):
    node = root
    for part in _parts(path):
        child = node.children.get(part)
        if child is None:
            if not create:
                return None
            child = Node(part, mode, root.mtime)
            node.children[part] = child
        node = child
    return node

def _parent(root, path):
    parts = _parts(path)
    return node_at(root, b'/'.join(parts[:-1]).decode(), create=True), parts[-1]

def add_file(root, path, source, mode=0o100644, uid=0, gid=0):
    parent, name = _parent(root, path)
    node = Node(name, mode, os.path.getmtime(long_path(source)), source=str(source))
    node.uid, node.gid = uid, gid
    parent.children[name] = node
    return node

def add_symlink(root, path, target, mode=0o120777):
    parent, name = _parent(root, path)
    parent.children[name] = Node(name, mode, root.mtime, target=os.fsencode(target))

def remove(root, path):
    parent, name = _parent(root, path)
    return parent.children.pop(name, None)

def _merge(into, other):
    for name, child in other.children.items():
        mine = into.children.get(name)
        if mine is not None and stat.S_ISDIR(mine.mode) and stat.S_ISDIR(child.mode):
            _merge(mine, child)
        else:
            into.children[name] = child

def graft(root, path, subtree):
    parent, name = _parent(root, path)
    existing = parent.children.get(name)
    if existing is not None and stat.S_ISDIR(existing.mode) and stat.S_ISDIR(subtree.mode):
        _merge(existing, subtree)
        return existing
    subtree.name = name
    parent.children[name] = subtree
    return subtree

def move(root, source, destination):
    node = remove(root, source)
    if node is not None:
        graft(root, destination, node)
    return node

def scan_tar(tar_path, scratch):
    import tarfile
    os.makedirs(scratch, exist_ok=True)
    root = Node(b'', 0o40755, 0)
    count = 0
    with tarfile.open(tar_path) as tar:
        for member in tar:
            parts = _parts(member.name)
            if not parts:
                root.mode = stat.S_IFDIR | (member.mode & 0o7777)
                continue
            path = b'/'.join(parts).decode()
            if member.isdir():
                node = node_at(root, path, create=True)
                node.mode, node.uid, node.gid, node.mtime = stat.S_IFDIR | (member.mode & 0o7777), member.uid, member.gid, int(member.mtime)
            elif member.issym():
                add_symlink(root, path, member.linkname, stat.S_IFLNK | (member.mode & 0o7777 or 0o777))
            elif member.isfile() or member.islnk():
                count += 1
                source = os.path.join(scratch, str(count))
                data = tar.extractfile(member).read()
                with open(source, 'wb') as out:
                    out.write(data)
                node = add_file(root, path, source, stat.S_IFREG | (member.mode & 0o7777), member.uid, member.gid)
                node.mtime = int(member.mtime)
    root.mtime = max([root.mtime] + [int(time.time())])
    return root

def sources_under(root, path):
    node = node_at(root, path)
    found = set()
    stack = [node] if node else []
    while stack:
        current = stack.pop()
        if current.source:
            found.add(current.source)
        stack.extend(current.children.values())
    return found

def pack(root, output, block=16384, zstd=None, compress=None, raw=(), page=None):
    if zstd is not None:
        return build_chunked(root, block, output, zstd, zstd=True, raw_sources=set(raw))
    if compress is not None:
        return build_chunked(root, block, output, compress, raw_sources=set(raw))
    return build(root, block, output, page)

def main():
    parser = argparse.ArgumentParser(description='')
    parser.add_argument('root')
    parser.add_argument('-o', '--output', default='root.packfs')
    parser.add_argument('-m', '--manifest', help='owners, modes and extra symlinks/directories (see above)')
    parser.add_argument('-b', '--block', type=lambda v: int(v, 0), default=16384,
                        help='file data alignment, a power of two >= 4096 (default 16384)')
    parser.add_argument('-z', '--compress', type=int, nargs='?', const=9, default=None, metavar='LEVEL',
                        help='store every file in LZ4-compressed chunks (an image for a RAM disk only)')
    parser.add_argument('--zstd', type=int, nargs='?', const=15, default=None, metavar='LEVEL',
                        help='the same with Zstandard: smaller, slower to read')
    parser.add_argument('--raw-prefix', action='append', default=[], help='keep files below this root-relative directory uncompressed')
    parser.add_argument('-a', '--page-align', type=lambda v: int(v, 0), default=0, metavar='SIZE',
                        help='align files of at least SIZE bytes to SIZE (a multiple of the block size); '
                             'use -b 4096 -a 16384 for images with many small files')
    args = parser.parse_args()
    if args.block < 4096 or args.block & (args.block - 1):
        sys.exit('mkpackfs: block size must be a power of two, at least 4096')
    if not os.path.isdir(args.root):
        sys.exit(f'mkpackfs: {args.root} is not a directory')

    root, skipped = scan(args.root)
    added = apply_manifest(root, args.manifest) if args.manifest else 0
    if args.page_align and args.page_align % args.block:
        sys.exit('mkpackfs: the page alignment must be a multiple of the block size')
    raw_sources = set()
    for prefix in args.raw_prefix:
        target = os.path.abspath(os.path.join(args.root, prefix))
        if os.path.commonpath([os.path.abspath(args.root), target]) != os.path.abspath(args.root):
            parser.error('raw-prefix must stay within the image root')
        raw_sources.update(n.source for n in layout(root) if n.source and
                           os.path.commonpath([os.path.abspath(n.source), target]) == target)
    if args.zstd is not None:
        size, count = build_chunked(root, args.block, args.output, args.zstd, zstd=True, raw_sources=raw_sources)
    elif args.compress is not None:
        size, count = build_chunked(root, args.block, args.output, args.compress, raw_sources=raw_sources)
    else:
        size, count = build(root, args.block, args.output, args.page_align or None)
    for path in skipped:
        print(f'mkpackfs: skipped special file {path}', file=sys.stderr)
    extra = f' ({added} from the manifest alone)' if added else ''
    print(f'{args.output}: {count} nodes{extra}, {size // 1024} KB')

if __name__ == '__main__':
    main()
