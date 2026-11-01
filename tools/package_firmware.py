"""Wrap an ESP-IDF app image in the Z-lamp OTA envelope (never bootloader/NVS)."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def package(source, destination):
    data = Path(source).read_bytes()
    if len(data) < 288 or data[0] != 0xE9 or struct.unpack_from('<I', data, 32)[0] != 0xABCD5432:
        raise ValueError('Not an ESP-IDF application image')
    version, project, elf = data[48:80], data[80:112], data[176:208]
    name = project.split(b'\0', 1)[0].decode('ascii')
    text_version = version.split(b'\0', 1)[0].decode('ascii')
    if tuple(map(int, text_version.split('.')[:2])) < (2, 7):
        raise ValueError('Pre-OTA firmware cannot be packaged; minimum version is 2.7')
    board = {'lamp_gateway': 1, 'hue_lamp': 2}[name]
    if len(data) > (0x1E0000 if board == 1 else 0x180000):
        raise ValueError('Application exceeds its OTA slot')
    header = b'ZLAMPOTA' + bytes((1, board, 0, 0)) + struct.pack('<I', len(data))
    header += hashlib.sha256(data).digest() + project + version + elf
    assert len(header) == 144
    dest = Path(destination)
    dest.write_bytes(header + data)
    manifest = {'board': name, 'version': text_version, 'bytes': len(data),
                'sha256': hashlib.sha256(data).hexdigest(), 'firmwareId': elf.hex(), 'package': dest.name}
    dest.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'OTA package: {dest.name}, {text_version}, {len(data)} bytes')
    return header


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('output')
    args = parser.parse_args()
    package(args.image, args.output)
