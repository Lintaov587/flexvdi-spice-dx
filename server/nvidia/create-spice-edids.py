#!/usr/bin/env python3
"""Create four uniquely identified 1080p virtual monitors for the NVIDIA X driver."""
from pathlib import Path

source = Path('/etc/X11/spice-virtual.edid').read_bytes()
if source[:8] != bytes.fromhex('00 ff ff ff ff ff ff 00'):
    raise ValueError('Invalid source EDID')
for number in range(1, 5):
    edid = bytearray(source[:128])
    edid[10:12] = number.to_bytes(2, 'little')
    edid[12:16] = number.to_bytes(4, 'little')
    edid[18:20] = bytes([1, 3])
    edid[21:23] = bytes([51, 29])
    edid[24] |= 2  # First detailed timing is preferred.
    edid[35:38] = bytes(3)
    edid[38:54] = bytes.fromhex('81 c0 a9 c0 d1 c0 01 01 01 01 01 01 01 01 01 01')
    edid[54:72] = bytes.fromhex('02 3a 80 18 71 38 2d 40 58 2c 45 00 fd 1e 11 00 00 1e')
    edid[72:90] = bytes.fromhex('01 1d 00 72 51 d0 1e 20 6e 28 55 00 fd 1e 11 00 00 1e')
    edid[90:108] = bytes.fromhex('00 00 00 fc 00') + f'SPICE-{number}\n'.ljust(13).encode('ascii')
    edid[108:126] = bytes.fromhex('00 00 00 fd 00 32 4b 1e 5a 11 00 0a 20 20 20 20 20 20')
    edid[126] = 0
    edid[127] = (-sum(edid[:127])) & 255
    Path(f'/etc/X11/spice-virtual-{number}.edid').write_bytes(edid)
    print(f'SPICE-{number}: 128 bytes, checksum {sum(edid) % 256}')
