"""Hardware smoke check for the flashed XIAO; resets native USB, never writes flash."""
import argparse
from pathlib import Path
import re
import time

import serial
from serial.tools import list_ports
from esptool.reset import HardReset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--seconds', type=int, default=20)
    args = parser.parse_args()
    if not 10 <= args.seconds <= 60:
        parser.error('--seconds must be between 10 and 60')
    device = next((p for p in list_ports.comports() if p.device.lower() == args.port.lower()), None)
    if device is None or (device.vid, device.pid) != (0x303A, 0x1001):
        parser.error('Select the XIAO native USB Serial/JTAG port (303A:1001)')

    port = serial.Serial(port=None, baudrate=115200, timeout=0.25)
    port.dtr = False
    port.rts = False
    port.port = args.port
    chunks = []
    with port:
        port.reset_input_buffer()
        HardReset(port, uses_usb=True)()
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            chunk = port.read(port.in_waiting or 1)
            if chunk:
                chunks.append(chunk)

    output = b''.join(chunks).decode('utf-8', errors='replace').replace('\r', '')
    output = re.sub(r'\x1b\[[0-9;]*m', '', output)
    log = Path(__file__).resolve().parents[1] / 'build/coordinator/boot-test.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(output, encoding='utf-8')
    ready = re.search(r'Zigbee network (?:formed|restored); channel (\d+)', output)
    failed = re.search(r'Guru Meditation|panic\x27ed|assert failed|ESP_ERROR_CHECK failed|Rebooting', output)
    if failed or output.count('Calling app_main()') != 1 or not ready:
        raise SystemExit(f'FAIL: startup, Zigbee readiness or stability check; inspect {log}')
    print(f'PASS: one startup, Zigbee channel {ready.group(1)}, no crash during {args.seconds} s; USB port released.')
    print(f'Log: {log}')


if __name__ == '__main__':
    main()
