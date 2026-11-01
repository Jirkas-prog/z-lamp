"""Read-only release checks: app envelopes, flash bounds and preservation of existing data."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def partitions(data):
    result = {}
    for offset in range(0, len(data), 32):
        entry = data[offset:offset + 32]
        if len(entry) != 32 or entry[:2] != b'\xaa\x50':
            break
        _, kind, subtype, address, size, name, flags = struct.unpack('<HBBII16sI', entry)
        name = name.split(b'\0')[0].decode('ascii')
        assert name not in result
        result[name] = (kind, subtype, address, size, flags)
    assert result
    return result


def verify(board, backup):
    folder = ROOT / 'build' / board
    is_c6 = board == 'coordinator'
    name = 'hue_lamp' if is_c6 else 'lamp_gateway'
    table = partitions((folder / 'partition_table/partition-table.bin').read_bytes())
    ordered = sorted(table.values(), key=lambda p: p[2])
    end = 0x9000
    for kind, subtype, address, size, flags in ordered:
        assert address >= end and address % 4096 == 0 and size % 4096 == 0
        end = address + size
        assert end <= 0x400000 and flags == 0
    assert table['nvs'][:4] == (1, 2, 0x9000, 0x6000)
    assert table['phy_init'][:4] == (1, 1, 0xf000, 0x1000)
    slot = 0x180000 if is_c6 else 0x1e0000
    assert table['ota_0'][:4] == (0, 0x10, 0x10000, slot)
    assert table['ota_1'][:4] == (0, 0x11, 0x1a0000 if is_c6 else 0x1f0000, slot)
    assert table['otadata'][:4] == (1, 0, 0x195000 if is_c6 else 0x3d0000, 0x2000)
    if is_c6:
        assert table['zb_storage'][:4] == (1, 0x81, 0x190000, 0x4000)
        assert table['zb_fct'][:4] == (1, 0x81, 0x194000, 0x1000)
    if backup:
        # Backup begins at flash 0x8000 (partition table followed by NVS and PHY).
        with Path(backup).open('rb') as stream:
            old = partitions(stream.read(0x1000))
        assert old.get('factory', old.get('ota_0'))[2] == table['ota_0'][2]
        for label, entry in old.items():
            if entry[0] == 1:
                assert table[label] == entry, f'Data partition changed: {label}'

    app = (folder / f'{name}.bin').read_bytes()
    envelope = (folder / f'{name}.zota').read_bytes()
    assert envelope[:12] == b'ZLAMPOTA' + bytes((1, 2 if is_c6 else 1, 0, 0))
    assert struct.unpack_from('<I', envelope, 12)[0] == len(app) <= slot
    assert envelope[144:] == app and envelope[16:48] == hashlib.sha256(app).digest()
    assert app[0] == 0xe9 and struct.unpack_from('<H', app, 12)[0] == (13 if is_c6 else 0)
    assert envelope[48:80] == app[80:112] and envelope[80:112] == app[48:80]
    assert envelope[112:144] == app[176:208]
    manifest = json.loads((folder / f'{name}.json').read_text())
    assert manifest['sha256'] == hashlib.sha256(app).hexdigest()
    assert manifest['firmwareId'] == app[176:208].hex()

    flash = json.loads((folder / 'flasher_args.json').read_text())
    files = {int(address, 16): filename for address, filename in flash['flash_files'].items()}
    assert files == {0 if is_c6 else 0x1000: 'bootloader/bootloader.bin',
                     0x8000: 'partition_table/partition-table.bin', 0x10000: f'{name}.bin',
                     table['otadata'][2]: 'ota_data_initial.bin'}
    for address, filename in files.items():
        size = (folder / filename).stat().st_size
        end = address + (size + 4095) // 4096 * 4096
        for label, (kind, _, start, length, _) in table.items():
            if kind == 1 and label != 'otadata':
                assert end <= start or address >= start + length, f'Flash write overlaps {label}'
    assert (folder / 'ota_data_initial.bin').read_bytes() == b'\xff' * 0x2000
    config = ROOT / ('gateway/sdkconfig' if not is_c6 else 'sdkconfig')
    assert 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' in config.read_text()
    if not is_c6:
        settings = config.read_text().splitlines()
        assert 'CONFIG_BT_NIMBLE_SM_LVL=2' in settings, 'Just Works must not require MITM authentication'
        assert 'CONFIG_BT_NIMBLE_SM_SC=y' in settings
        assert '# CONFIG_BT_NIMBLE_SM_LEGACY is not set' in settings
        cmake = (ROOT / 'gateway/main/CMakeLists.txt').read_text()
        assert 'MYNEWT_VAL_BLE_SM_SC_ONLY=1' not in cmake, 'SC_ONLY stalls incoming pairing in IDF 5.3.1'
    print(f'{board}: {manifest["version"]}, {len(app)} / {slot} bytes; package, slots, flash writes and data preservation verified.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--coordinator-backup')
    parser.add_argument('--gateway-backup')
    args = parser.parse_args()
    verify('coordinator', args.coordinator_backup)
    verify('gateway', args.gateway_backup)
