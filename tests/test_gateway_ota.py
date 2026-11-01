"""Live, opt-in T-Display OTA test: writes the given firmware and restores temporary IR settings."""
import argparse
import http.client
import json
from pathlib import Path
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REMOTE_KEYS = ('whiteKelvin', 'whiteLevel', 'brightnessSteps', 'brightnessLevels',
               'customBrightness', 'irMembers', 'buttons')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--firmware', required=True, type=Path)
    parser.add_argument('--before-web', type=Path, help='Optional private pre-flash snapshot to verify preserved settings')
    parser.add_argument('--https-probe', help='Optional public HTTPS text file: verify TLS download and safe rejection as non-firmware')
    args = parser.parse_args()
    base = args.url.rstrip('/')

    def api(name, body=None, timeout=10):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(base + '/api/' + name, data=data,
                                         headers={} if data is None else {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.load(response)

    def until(check, seconds=20):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                result = check()
                if result:
                    return result
            except (urllib.error.URLError, TimeoutError, http.client.HTTPException):
                pass
            time.sleep(1)
        raise AssertionError('Timed out waiting for hardware state')

    def saved():
        return not api('state')['saving']

    payload = args.firmware.read_bytes()
    assert payload[:12] == b'ZLAMPOTA\x01\x01\x00\x00', 'Expected T-Display package'
    identity = payload[112:144].hex()
    before = api('update/status')
    assert not before['busy'] and before['gateway']['confirmed']
    for route, filename in (('/', 'index.html'), ('/app.js', 'app.js'), ('/style.css', 'style.css')):
        with urllib.request.urlopen(base + route, timeout=10) as response:
            assert response.read() == (ROOT / 'gateway/main/web' / filename).read_bytes(), filename
    print('PASS: all three embedded web assets match source.', flush=True)
    settings_before, scenes_before = api('settings'), api('scenes')
    remote_before = {key: scenes_before[key] for key in REMOTE_KEYS}
    state_before = api('state')
    try:
        # Reject non-increasing steps atomically; do not operate any light.
        try:
            api('remote', {'customBrightness': True, 'brightnessSteps': 2, 'brightnessLevels': [1, 1]})
            raise AssertionError('Invalid brightness scale accepted')
        except urllib.error.HTTPError as error:
            assert error.code == 400 and json.load(error)['error'] == 'invalid_remote'
        assert {key: api('scenes')[key] for key in REMOTE_KEYS} == remote_before
        members = [light['id'] for light in state_before['lights'] if light['id'] != 'display'][:1]
        trial = {'customBrightness': True, 'brightnessSteps': 4, 'brightnessLevels': [1, 2, 4, 20], 'irMembers': members}
        api('remote', trial)
        until(saved)
        assert all(api('scenes')[key] == value for key, value in trial.items())
        print('PASS: custom steps and selected members saved; invalid scale rejected.', flush=True)

        address = urllib.parse.urlsplit(base)
        connection = http.client.HTTPConnection(address.hostname, address.port or 80, timeout=15)
        try:
            connection.putrequest('POST', '/api/update/gateway')
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
        assert status['gateway']['boot'] == before['gateway']['boot']
        assert status['gateway']['id'] == before['gateway']['id']
        print('PASS: interrupted transfer cancelled; original firmware still running.', flush=True)

        request = urllib.request.Request(base + '/api/update/gateway', data=payload,
                                         headers={'Content-Type': 'application/octet-stream'})
        print('Uploading complete gateway firmware over Wi-Fi...', flush=True)
        with urllib.request.urlopen(request, timeout=180) as response:
            assert json.load(response)['ok']

        def confirmed():
            status = api('update/status')
            current = status['gateway']
            if current['boot'] != before['gateway']['boot'] and current['id'] == identity and current['confirmed']:
                return status
            return None

        status = until(confirmed, 100)
        assert api('settings') == settings_before
        actual = api('scenes')
        assert all(actual[key] == value for key, value in trial.items())
        print('PASS: Wi-Fi OTA restarted and confirmed expected firmware; IR settings persisted.', flush=True)
        if args.https_probe:
            boot = status['gateway']['boot']
            api('update/url', {'target': 'gateway', 'url': args.https_probe})
            until(lambda: not api('update/status')['busy'], 50)
            status = api('update/status')
            assert status['error'] == 'update_header', status['error']
            assert status['gateway']['boot'] == boot
            print('PASS: HTTPS/TLS download reached header validation; non-firmware safely rejected.', flush=True)
    finally:
        api('remote', remote_before)
        until(saved)
    assert {key: api('scenes')[key] for key in REMOTE_KEYS} == remote_before
    if args.https_probe:
        # Clear the intentional bad-package status and verify a normal reboot of the accepted OTA image.
        boot = api('update/status')['gateway']['boot']
        api('reboot', {})
        until(lambda: (s := api('update/status'))['gateway']['boot'] != boot and s['gateway']['confirmed'], 60)
        until(lambda: len(api('state')['lights']) == len(state_before['lights']))
    if args.before_web:
        baseline = json.loads(args.before_web.read_text(encoding='utf-8'))
        assert api('settings') == baseline['settings'], 'Pre-flash settings changed'
        scenes = api('scenes')
        assert all(scenes[key] == value for key, value in baseline['scenes'].items()), 'Pre-flash scenes changed'
        state = api('state')
        assert state['groups'] == baseline['state']['groups']
        assert sorted((l['id'], l['name']) for l in state['lights']) == sorted((l['id'], l['name']) for l in baseline['state']['lights'])
        assert state['apEnabled'] == baseline['state']['apEnabled']
        print('PASS: pre-flash Wi-Fi/alias/IR/presets/scenes/groups/light names preserved.', flush=True)
    print('PASS: temporary settings restored and saved; no bulb commands issued.', flush=True)


if __name__ == '__main__':
    main()
