"""List or extract entries from a ThirdWire .cat archive (SF2, July 2013 format).

Format (all little-endian; see docs/option-trace.md):
  0x00  wchar[4]  L"CAT\\0"
  0x08  u32       version (4)
  0x0c  u32       entry count
  0x10  u32 x7    unknown (flags / hash; not needed for reading)
  0x2c  entries:  u32 offset, u32 size, u32 name_bytes, wchar name[name_bytes/2] (NUL-terminated)
Entry data is stored uncompressed at the absolute file offset.

Usage:
  python tools/catx.py list    <file.cat> [substring]
  python tools/catx.py extract <file.cat> <out_dir> [substring]

Extracted game data must stay outside the repository (EULA 3(d)).
"""
import os
import struct
import sys

HEADER_SIZE = 0x2c


def read_entries(data):
    if data[:8] != 'CAT\0'.encode('utf-16-le'):
        raise ValueError('not a CAT archive')
    version, count = struct.unpack_from('<II', data, 8)
    if version != 4:
        raise ValueError(f'unsupported CAT version {version}')
    pos, entries = HEADER_SIZE, []
    for _ in range(count):
        offset, size, name_bytes = struct.unpack_from('<III', data, pos)
        pos += 12
        name = data[pos:pos + name_bytes].decode('utf-16-le').rstrip('\0')
        pos += name_bytes
        if offset + size > len(data):
            raise ValueError(f'entry {name} runs past end of file')
        entries.append((name, offset, size))
    return entries


def main(argv):
    if len(argv) < 3 or argv[1] not in ('list', 'extract'):
        sys.exit(__doc__)
    data = open(argv[2], 'rb').read()
    entries = read_entries(data)
    if argv[1] == 'list':
        needle = argv[3].upper() if len(argv) > 3 else ''
        for name, offset, size in entries:
            if needle in name.upper():
                print(f'{offset:10d} {size:10d}  {name}')
        return
    if len(argv) < 4:
        sys.exit(__doc__)
    out_dir, needle = argv[3], (argv[4].upper() if len(argv) > 4 else '')
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
    if os.path.abspath(out_dir).startswith(root):
        sys.exit('refusing to extract game data into the repository')
    os.makedirs(out_dir, exist_ok=True)
    n = 0
    for name, offset, size in entries:
        if needle in name.upper():
            safe = os.path.basename(name.replace('\\', '/'))
            with open(os.path.join(out_dir, safe), 'wb') as f:
                f.write(data[offset:offset + size])
            n += 1
    print(f'extracted {n} entries to {out_dir}')


if __name__ == '__main__':
    main(sys.argv)
