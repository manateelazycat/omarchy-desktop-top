#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 ManateeLazyCat

"""Verify real service/worker IPC and desktop occupancy without user writes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def wait_until(check, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError('Timed out waiting for worker state')


def main():
    original = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config'))) / 'btop/btop.conf'
    original_hash = hashlib.sha256(original.read_bytes()).hexdigest() if original.exists() else None
    with tempfile.TemporaryDirectory(prefix='desktop-top-bridge-') as directory:
        runtime = Path(directory)
        plugin = runtime / 'Plugin'
        plugin.mkdir()
        shutil.copy2(ROOT / 'Service.qml', plugin / 'Service.qml')
        shutil.copytree(ROOT / 'renderer', plugin / 'renderer')
        (plugin / 'qml').symlink_to(ROOT / 'build/qml', target_is_directory=True)
        shutil.copy2(ROOT / 'tests/Bridge.qml', runtime / 'shell.qml')
        (runtime / 'Commons').symlink_to('/usr/share/omarchy/shell/Commons', target_is_directory=True)
        config = runtime / 'config/omarchy'
        config.mkdir(parents=True)
        (config / 'shell.json').write_text(json.dumps({'plugins': [{
            'id': 'io.github.manateelazycat.desktop-top', 'positionX': 0.31, 'positionY': 0.69,
            'width': 1050, 'height': 680}]}))
        output = ROOT / 'tests/output'
        output.mkdir(exist_ok=True)
        log_path = output / 'bridge.log'
        env = dict(os.environ, XDG_CONFIG_HOME=str(runtime / 'config'))
        with log_path.open('w') as log:
            process = subprocess.Popen(['quickshell', '-p', str(runtime), '--no-color'], env=env,
                                       stdout=log, stderr=subprocess.STDOUT)

            def call(target, method, *args):
                result = subprocess.run(['qs', 'ipc', '--pid', str(process.pid), 'call', target, method, *args],
                                        capture_output=True, text=True, timeout=3)
                return result.stdout.strip() if result.returncode == 0 else ''

            def status():
                try:
                    return json.loads(call('desktop-top', 'status') or '{}')
                except json.JSONDecodeError:
                    return {}

            worker_pid = None
            btop_pids = []
            try:
                current = wait_until(lambda: (s if (s := status()).get('connected') and s.get('screens') else None))
                assert current['version'] == json.loads((ROOT / 'manifest.json').read_text())['version']
                expected_palette = json.loads(call('bridge-test', 'info'))['palette']
                assert current['palette'] == expected_palette, (current['palette'], expected_palette)
                assert current['positionX'] == 0.31 and current['positionY'] == 0.69
                assert current['width'] == 1050 and current['height'] == 680
                worker_pid = current['workerPid']
                wait_until(lambda: all(s['running'] for s in status()['screens'] if s['visible']))
                current = status()
                assert all(s['visible'] == current['emptyScreens'].get(s['name'], False) for s in current['screens'])
                layers = json.loads(subprocess.check_output(['hyprctl', '-j', 'layers']))
                mapped = [monitor for monitor, data in layers.items() for level in data['levels'].values()
                          for layer in level if layer.get('pid') == worker_pid and layer.get('namespace') == 'omarchy-desktop-top']
                assert sorted(mapped) == sorted(s['name'] for s in current['screens'] if s['visible'])
                btop_pids = [s['btopPid'] for s in current['screens'] if s['running']]
                print(f'PASS: host/worker IPC restores geometry; {len(mapped)} empty monitors have real btop layers')
                for pid in btop_pids:
                    assert Path(f'/proc/{pid}/comm').read_text().strip() == 'btop'
                    assert f'PPid:\t{worker_pid}\n' in Path(f'/proc/{pid}/status').read_text()
                assert call('desktop-top', 'setPosition', '0.2', '0.4') == 'ok'
                wait_until(lambda: status().get('positionX') == 0.2)
                assert call('desktop-top', 'setSize', '1120', '720') == 'ok'
                wait_until(lambda: status().get('width') == 1120 and status().get('height') == 720)
                assert call('desktop-top', 'setSize', 'invalid', '-1').startswith('Size must')
                call('bridge-test', 'theme')
                wait_until(lambda: status().get('palette', {}).get('accent') == '#f7768e')
                assert status()['palette']['foreground'] == '#eeeeee'
                assert json.loads(call('bridge-test', 'info'))['writes'] == 2
                time.sleep(0.5)
                current = status()
                assert [s['btopPid'] for s in current['screens'] if s['running']] == btop_pids
                print('PASS: settings and live theme cross IPC without replacing terminal processes')
                visible = next((s for s in current['screens'] if s['visible']), None)
                if visible:
                    name = visible['name']
                    assert call('desktop-top', 'minimize', name) == 'ok'
                    wait_until(lambda: any(s['name'] == name and s['minimized'] and s['tabVisible']
                                          and not s['visible'] for s in status()['screens']))
                    assert [s['btopPid'] for s in status()['screens'] if s['running']] == btop_pids
                    assert call('desktop-top', 'restore', name) == 'ok'
                    wait_until(lambda: any(s['name'] == name and s['visible'] and not s['minimized']
                                          for s in status()['screens']))
                    assert json.loads(call('bridge-test', 'info'))['writes'] == 4
                    print('PASS: minimize/restore IPC keeps all btop processes and saves each transition once')
                assert call('desktop-top', 'minimize', 'unknown-monitor') == 'Unknown screen.'
                call('bridge-test', 'unload')
                wait_until(lambda: not Path(f'/proc/{worker_pid}').exists())
                wait_until(lambda: all(not Path(f'/proc/{pid}').exists() for pid in btop_pids))
                print('PASS: unloading the plugin terminates its worker and all btop processes')
                call('bridge-test', 'quit')
                process.wait(timeout=5)
                assert 'TypeError' not in log_path.read_text() and 'ReferenceError' not in log_path.read_text()
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                if worker_pid:
                    wait_until(lambda: not Path(f'/proc/{worker_pid}').exists())
    if original_hash:
        assert hashlib.sha256(original.read_bytes()).hexdigest() == original_hash
    print('PASS: the user btop config is unchanged')


if __name__ == '__main__':
    main()
