"""Read-only GOD input, bounded XDVDFS extraction; never overwrite outputs.

SVOD block layout checked against raburton/god2iso Form1.cs.
Only supports a volume descriptor at logical 0x10000 and unshifted sectors.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct

GROUP = 0xCC000
PART = GROUP * 0xCB

class GodReader:
    def __init__(self, container):
        self.folder = Path(str(container) + '.data')
        self.handles = {}

    def read(self, offset, size):
        if offset < 0 or size < 0:
            raise ValueError('Negative range')
        result = bytearray()
        while size:
            part, local = divmod(offset, PART)
            group, within = divmod(local, GROUP)
            count = min(size, GROUP - within)
            if part not in self.handles:
                self.handles[part] = (self.folder / f'Data{part:04d}').open('rb')
            stream = self.handles[part]
            stream.seek(0x2000 + group * (GROUP + 0x1000) + within)
            block = stream.read(count)
            if len(block) != count:
                raise EOFError(f'Truncated Data{part:04d}')
            result.extend(block)
            size -= count
            offset += count
        return bytes(result)

    def close(self):
        for stream in self.handles.values():
            stream.close()

def inventory(reader):
    descriptor = reader.read(0x10000, 2048)
    if descriptor[:20] != b'MICROSOFT*XBOX*MEDIA' or descriptor[-20:] != descriptor[:20]:
        raise ValueError('Unsupported or invalid volume descriptor')
    root_sector, root_size = struct.unpack_from('<II', descriptor, 20)
    files, visited_dirs = [], set()

    def directory(sector, size, parent):
        if (sector, size) in visited_dirs or size > 16 * 1024 * 1024:
            raise ValueError('Directory cycle or oversized table')
        visited_dirs.add((sector, size))
        table = reader.read(sector * 2048, size)
        seen = set()
        def node(offset):
            if offset in seen or offset + 14 > len(table):
                raise ValueError('Invalid directory node')
            seen.add(offset)
            left, right, start, length, flags, n = struct.unpack_from('<HHIIBB', table, offset)
            if offset + 14 + n > len(table) or not n:
                raise ValueError('Invalid filename length')
            name = table[offset+14:offset+14+n].decode('ascii')
            if name in ('.', '..') or any(c in name for c in '/\\:'):
                raise ValueError('Unsafe filename')
            path = parent / name
            if left:
                node(left * 4)
            if flags & 0x10:
                if length:
                    directory(start, length, path)
            else:
                files.append({'path': path.as_posix(), 'sector': start, 'size': length})
            if right:
                node(right * 4)
        if size:
            node(0)
    directory(root_sector, root_size, Path())
    if len({f['path'].lower() for f in files}) != len(files):
        raise ValueError('Duplicate paths')
    return files

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('container', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--list-only', action='store_true')
    args = parser.parse_args()
    reader = GodReader(args.container)
    try:
        files = inventory(reader)
        print(f'{len(files)} files, {sum(f["size"] for f in files)} bytes', flush=True)
        if args.list_only:
            print(json.dumps(files[:30], indent=2))
            return
        args.output.mkdir(parents=True, exist_ok=False)
        for i, entry in enumerate(files):
            target = args.output / entry['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            digest = hashlib.sha256()
            with target.open('xb') as stream:
                for offset in range(0, entry['size'], 4 * 1024 * 1024):
                    data = reader.read(entry['sector'] * 2048 + offset, min(4 * 1024 * 1024, entry['size'] - offset))
                    stream.write(data)
                    digest.update(data)
            entry['sha256'] = digest.hexdigest()
            if i % 100 == 0:
                print(f'{i+1}/{len(files)}: {entry["path"]}', flush=True)
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(files, indent=2), encoding='utf-8')
        print('Extraction complete', flush=True)
    finally:
        reader.close()

if __name__ == '__main__':
    main()
