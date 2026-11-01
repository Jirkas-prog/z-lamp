"""Restart the flashed T-Display and verify startup and the persisted manual AP mode."""
import argparse
from pathlib import Path
import re
import time
import serial
from esptool.reset import HardReset

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', required=True)
parser.add_argument('--ap', choices=['on', 'off', 'saved'], default='off')
parser.add_argument('--seconds', type=int, default=30)
parser.add_argument('--alias', help='Expected active device alias, when checking a particular configuration')
args = parser.parse_args()
if not 10 <= args.seconds <= 60:
    parser.error('--seconds must be between 10 and 60')
port = serial.Serial(port=None, baudrate=115200, timeout=0.25)
port.dtr = False
port.rts = False
port.port = args.port
chunks = []
with port:
    port.reset_input_buffer()
    HardReset(port, uses_usb=False)()
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            chunks.append(chunk)
output = b''.join(chunks).decode('utf-8', errors='replace').replace('\r', '')
output = re.sub(r'\x1b\[[0-9;]*m', '', output)
log = Path(__file__).resolve().parents[1] / 'build/gateway/boot-test.log'
log.parent.mkdir(parents=True, exist_ok=True)
log.write_text(output, encoding='utf-8')
assert not re.search(r'GPIO\[(32|33|36|39)\]|Pots ADC', output), 'Unexpected unused input initialization'
assert 'Receiver supply configured: GPIO27=HIGH, GPIO26=LOW' in output, 'Missing IR power setup'
assert 'NEC receiver ready on GPIO25' in output, 'Missing IR receiver initialization'
assert re.search(r'network: Alias=.+; hostname=.+; web=http://.+\.local', output), 'Missing network alias/mDNS initialization'
if args.alias:
    assert f'Alias={args.alias}; hostname={args.alias.lower()}; web=http://{args.alias.lower()}.local' in output, 'Unexpected network alias'
assert output.index('Receiver supply configured:') < output.index('NEC receiver ready'), 'IR started before power setup'
failed = re.search(r"Guru Meditation|panic'ed|assert failed|ESP_ERROR_CHECK failed|Rebooting", output)
ready = re.search(r'Gateway ready; AP=.* \((on|off)\);', output)
expected = ready.group(1) if args.ap == 'saved' and ready else args.ap
ap_started = 'wifi:mode : softAP' in output or 'DHCP server started on interface WIFI_AP_DEF' in output
if failed or not ready or output.count('Calling app_main()') != 1 or ready.group(1) != expected or ap_started != (expected == 'on'):
    raise SystemExit(f'FAIL: startup or AP mode; inspect {log}')
print(f'PASS: single boot, AP {expected}, no crash for {args.seconds} s; {args.port} released.')
print(f'Log: {log}')
