"""Opt-in hardware test: install a C6 package through T-Display HTTP and the existing UART."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--firmware', required=True, type=Path)
    parser.add_argument('--before-web', type=Path, help='Private pre-flash snapshot; never printed')
    args = parser.parse_args()
    base = args.url.rstrip('/')

    def api(name, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(base + '/api/' + name, data=data,
                                         headers={} if data is None else {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.load(response)

    def until(check, seconds=25):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                result = check()
                if result:
                    return result
            except (urllib.error.URLError, TimeoutError, http.client.HTTPException):
                pass
            time.sleep(1)
        raise AssertionError('Timed out waiting for C6 hardware state')

    payload = args.firmware.read_bytes()
    assert payload[:12] == b'ZLAMPOTA\x01\x02\x00\x00', 'Expected C6 package'
    identity = payload[112:144].hex()
    before = api('update/status')
    assert not before['busy'] and before['gateway']['confirmed']
    assert before['coordinator']['available'] and before['coordinator']['confirmed']
    baseline = (json.loads(args.before_web.read_text(encoding='utf-8-sig')) if args.before_web else
                {key: api(key) for key in ('settings', 'scenes', 'state')})

    address = urllib.parse.urlsplit(base)
    connection = http.client.HTTPConnection(address.hostname, address.port or 80, timeout=15)
    try:
        connection.putrequest('POST', '/api/update/coordinator')
        connection.putheader('Content-Type', 'application/octet-stream')
        connection.putheader('Content-Length', str(len(payload)))
        connection.endheaders()
        connection.send(payload[:144 + 1024])

        def received():
            status = api('update/status')
            assert not status['error'], status['error']
            return status['received'] >= 1024

        until(received)
        assert api('update/cancel', {})['ok']
        response = connection.getresponse()
        assert response.status == 400 and json.load(response)['error'] == 'update_cancelled'
    finally:
        connection.close()
    until(lambda: not api('update/status')['busy'])
    status = api('update/status')
    assert status['coordinator']['boot'] == before['coordinator']['boot']
    assert status['coordinator']['id'] == before['coordinator']['id']
    print('PASS: partial UART transfer cancelled; original C6 firmware still running.', flush=True)

    def upload():
        request = urllib.request.Request(base + '/api/update/coordinator', data=payload,
                                         headers={'Content-Type': 'application/octet-stream'})
        with urllib.request.urlopen(request, timeout=360) as response:
            assert json.load(response)['ok']

    print('Uploading C6 firmware via Wi-Fi and UART...', flush=True)
    with ThreadPoolExecutor(max_workers=1) as executor:
        future = executor.submit(upload)
        last = None
        while not future.done():
            time.sleep(2)
            status = api('update/status')
            percent = 100 * status['received'] // max(1, status['total'])
            progress = (status['phase'], percent // 20)
            if progress != last:
                print(f"C6: {status['phase']}, {percent}% ({status['received']}/{status['total']})", flush=True)
                last = progress
        future.result()

    status = api('update/status')
    current = status['coordinator']
    assert status['phase'] == 'done' and not status['busy'] and not status['error'], status['error']
    assert status['received'] == status['total'] == len(payload) - 144
    assert current['available'] and current['confirmed'] and current['id'] == identity
    assert current['boot'] != before['coordinator']['boot']
    assert status['gateway'] == before['gateway'], 'T-Display unexpectedly restarted or changed'
    print('PASS: C6 image validated, restarted and confirmed; T-Display stayed running.', flush=True)

    until(lambda: len(api('state')['lights']) == len(baseline['state']['lights']))
    assert api('settings') == baseline['settings'], 'Settings changed'
    assert api('scenes') == baseline['scenes'], 'Scenes/remote configuration changed'
    state = api('state')
    for key in ('groups', 'channel', 'alias', 'apEnabled'):
        assert state[key] == baseline['state'][key], key
    assert state['connected'] and not state['lastError']
    keys = ('id', 'name', 'transition', 'legacyXy', 'startup')
    def devices(snapshot):
        return sorted(({key: light.get(key) for key in keys} for light in snapshot['lights']), key=lambda l: l['id'])
    assert devices(state) == devices(baseline['state']), 'Paired devices or their settings changed'
    print('PASS: Zigbee channel, paired devices, names, startup settings and web configuration preserved.', flush=True)
    print('PASS: no bulb control or pairing commands issued.', flush=True)


if __name__ == '__main__':
    main()
