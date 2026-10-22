import struct
from pathlib import Path

REGIONS = (('adsp', 1, 0x9F100000, 0x04B00000, 55, 53, 8204),
           ('adsp_dtb', 36, 0x9F080000, 0x80000, 3, 1, 4044))

def check(directory, verbose=True):
    directory = Path(directory)
    result = []
    for name, pas, base, capacity, expected_count, expected_loads, expected_meta in REGIONS:
        raw = (directory / (name + '.mdt')).read_bytes()
        assert raw[:6] == b'\x7fELF\x01\x01', name
        phoff = struct.unpack_from('<I', raw, 28)[0]
        entsize, count = struct.unpack_from('<HH', raw, 42)
        assert phoff == 52 and entsize == 32 and count == expected_count, name
        headers = [struct.unpack_from('<8I', raw, phoff + i * entsize) for i in range(count)]
        hashes = [s for s in headers if (s[6] >> 24) & 7 == 2]
        loads = [(i, s) for i, s in enumerate(headers) if s[0] == 1 and (s[6] >> 24) & 7 != 2 and s[5]]
        assert len(hashes) == 1 and len(loads) == expected_loads and headers[0][0] != 1
        metadata = headers[0][4] + hashes[0][4]
        assert headers[0][4] >= phoff + count * entsize and metadata == len(raw) == expected_meta
        assert any(s[6] & (1 << 27) for _, s in loads)
        lowest = min(s[3] for _, s in loads)
        highest = max(s[3] + s[5] for _, s in loads)
        span = ((highest + 4095) & ~4095) - lowest
        assert lowest - base == 0x700000 and 0 < span <= capacity
        files, total, bss = [directory / (name + '.mdt')], 0, 0
        for index, s in loads:
            dest = base + s[3] - lowest
            assert base <= dest and dest + s[5] <= base + capacity and s[4] <= s[5]
            if s[4]:
                f = directory / f'{name}.b{index:02}'
                assert f.stat().st_size == s[4], f
                files.append(f)
            total += s[4]
            bss += s[5] - s[4]
        line = (f'{name}: PAS={pas} phdrs={count} loads={len(loads)} '
                f'metadata={headers[0][4]}+{hashes[0][4]}={metadata} '
                f'relocation=-0x700000 destinations=[{base:#x},{base + highest - lowest:#x}) '
                f'span={span:#x} file_bytes={total} bss={bss} OK')
        if verbose:
            print(line)
        result.append((files, line))
    return result
