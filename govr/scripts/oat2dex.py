#!/usr/bin/env python3
"""Extract embedded dex files from an Android 7 OAT/ODEX (ELF) file."""
import sys, struct, zlib, hashlib
data = open(sys.argv[1], 'rb').read()
out = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1]
i, n = 0, 0
while True:
    i = data.find(b'dex\n03', i)
    if i < 0: break
    size = struct.unpack_from('<I', data, i + 32)[0]
    if 0x70 <= size <= len(data) - i:
        name = f'{out}.classes{n or ""}.dex'
        d = bytearray(data[i:i+size])
        # dex2oat may alter the embedded dex: recompute signature and checksum
        d[12:32] = hashlib.sha1(d[32:]).digest()
        struct.pack_into('<I', d, 8, zlib.adler32(bytes(d[12:])))
        open(name, 'wb').write(d); print(name, size); n += 1
        i += size
    else:
        i += 4
