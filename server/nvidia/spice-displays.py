#!/usr/bin/env python3
"""NVIDIA virtual monitors, controlled over a bounded SPICE port protocol."""
import json
import os
import re
import select
import shlex
import subprocess
import time
from pathlib import Path
from gi.repository import Gio, GLib

PORT = '/dev/virtio-ports/org.spice-space.displays.0'
ENV = dict(os.environ, DISPLAY=':0', XAUTHORITY='/run/user/1000/gdm/Xauthority')
STATE = Path('/var/lib/spice-displays/layout.json')
OUTPUTS = ('HDMI-0', 'DP-0', 'DP-2', 'DP-4')
DEFAULT = [dict(id=0, width=1600, height=900, auto_resize=True)]
settings = DEFAULT.copy()


def run(args):
    result = subprocess.run(args, env=ENV, text=True, capture_output=True, timeout=10)
    if result.returncode:
        raise RuntimeError(result.stderr.strip()[:1000] or '显示命令执行失败')
    return result.stdout


def primary_input_id(heads):
    # Same active-monitor order used by spice-vdagent 0.22.1's Mutter backend.
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    state = bus.call_sync('org.gnome.Mutter.DisplayConfig',
        '/org/gnome/Mutter/DisplayConfig', 'org.gnome.Mutter.DisplayConfig',
        'GetCurrentState', None, None, Gio.DBusCallFlags.NONE, 3000, None).unpack()
    names = [m[0][0] for m in state[1] if any(mode[6].get('is-current', False) for mode in m[1])]
    if not heads or set(names) != {h['name'] for h in heads}:
        raise ValueError('GNOME 正在同步显示布局')
    origin = next((h['name'] for h in heads if h['x'] == 0 and h['y'] == 0), None)
    if origin is None:
        raise ValueError('主显示器必须位于桌面原点')
    return names.index(origin)


def topology():
    text = run(['xrandr', '--query'])
    root = re.search(r'current (\d+) x (\d+)', text)
    if root is None:
        raise ValueError('X11 桌面尚未就绪')
    heads = []
    for line in text.splitlines():
        match = re.match(r'^(\S+) connected(?: primary)? (\d+)x(\d+)\+(\d+)\+(\d+)', line)
        if match:
            name, w, h, x, y = match.groups()
            if name in OUTPUTS:
                number = OUTPUTS.index(name)
                setting = next((s for s in settings if s['id'] == number), {})
                heads.append(dict(id=number, name=name, x=int(x), y=int(y), width=int(w),
                                  height=int(h), auto_resize=setting.get('auto_resize', True)))
    heads.sort(key=lambda h: (h['y'], h['x'], h['id']))
    input_id = primary_input_id(heads)
    for head in heads:
        head['input_id'] = input_id
    return dict(version=2, type='state', monitors=heads, width=int(root[1]), height=int(root[2]))


def validate(monitors):
    if not isinstance(monitors, list) or not 1 <= len(monitors) <= 4:
        raise ValueError('请保留 1–4 块显示器')
    clean, ids = [], set()
    for m in monitors:
        if not isinstance(m, dict):
            raise ValueError('显示器配置无效')
        number, w, h, auto = (m.get(k) for k in ('id', 'width', 'height', 'auto_resize'))
        if any(type(v) is not int for v in (number, w, h)) or type(auto) is not bool:
            raise ValueError('显示器参数类型错误')
        if number not in range(4) or number in ids or not 320 <= w <= 3840 or not 200 <= h <= 2160:
            raise ValueError('分辨率范围：320×200 至 3840×2160；显示器编号不能重复')
        ids.add(number)
        clean.append(dict(id=number, width=(w + 1) & ~1, height=(h + 1) & ~1, auto_resize=auto))
    return sorted(clean, key=lambda m: m['id'])


def ensure_mode(output, w, h):
    name = f'SPICE-{w}x{h}'
    text = run(['xrandr', '--query'])
    block = re.search(r'^' + re.escape(output) + r' (?:connected|disconnected).*?(?=^\S|\Z)', text, re.M | re.S)
    if block and re.search(r'^\s+' + re.escape(name) + r'\s', block[0], re.M):
        return name
    if not re.search(r'^\s*' + re.escape(name) + r'\s', text, re.M):
        # CVT-RB uses 8-pixel horizontal rounding; the synthetic output accepts
        # an exact even active width within that timing's blanking interval.
        modeline = next(line for line in run(['cvt', '-r', str(w), str(h), '60']).splitlines()
                        if line.startswith('Modeline'))
        timing = shlex.split(modeline)[2:]
        timing[1] = str(w)
        result = subprocess.run(['xrandr', '--newmode', name, *timing], env=ENV,
                                text=True, capture_output=True, timeout=10)
        if result.returncode and 'BadName' not in result.stderr:
            raise RuntimeError(result.stderr.strip()[:1000])
    run(['xrandr', '--addmode', output, name])
    return name


def apply_layout(monitors):
    columns = min(2, len(monitors))
    column_width = max(m['width'] for m in monitors[::columns])
    row_height = max(m['height'] for m in monitors[:columns])
    command, expected = ['xrandr'], {}
    active = {m['id'] for m in monitors}
    for i, m in enumerate(monitors):
        name = OUTPUTS[m['id']]
        mode = ensure_mode(name, m['width'], m['height'])
        x, y = (i % columns) * column_width, (i // columns) * row_height
        command += ['--output', name, '--mode', mode, '--pos', f'{x}x{y}']
        if i == 0:
            command += ['--primary']
        expected[m['id']] = (x, y, m['width'], m['height'])
    for number, name in enumerate(OUTPUTS):
        if number not in active:
            command += ['--output', name, '--off']
    run(command)
    for _ in range(50):
        try:
            actual = topology()
            observed = {m['id']: (m['x'], m['y'], m['width'], m['height']) for m in actual['monitors']}
            if observed == expected:
                return actual
        except (ValueError, GLib.Error):
            pass
        time.sleep(.05)
    raise RuntimeError('显示服务没有完成请求的布局')


def cleanup_modes(monitors):
    keep = {f"SPICE-{m['width']}x{m['height']}" for m in monitors}
    text = run(['xrandr', '--query'])
    attached, current = {}, None
    for line in text.splitlines():
        match = re.match(r'^(\S+) (?:connected|disconnected)', line)
        if match:
            current = match[1]
        mode = re.match(r'^\s+(SPICE-\d+x\d+)\s', line)
        if mode and current:
            attached.setdefault(mode[1], []).append(current)
    for mode, outputs in attached.items():
        if mode in keep:
            continue
        try:
            for output in outputs:
                run(['xrandr', '--delmode', output, mode])
            run(['xrandr', '--rmmode', mode])
        except RuntimeError as exc:
            print(f'Mode cleanup: {exc}', flush=True)


def configure(monitors, save=True):
    global settings
    clean = validate(monitors)
    previous = settings
    settings = clean
    try:
        result = apply_layout(clean)
    except Exception:
        settings = previous
        try:
            apply_layout(previous)
        except Exception as rollback:
            print(f'Layout rollback failed: {rollback}', flush=True)
        raise
    if save:
        temporary = STATE.with_suffix('.tmp')
        temporary.write_text(json.dumps(dict(monitors=clean)))
        temporary.replace(STATE)
    cleanup_modes(clean)
    return result


def serve():
    global settings
    saved = json.loads(STATE.read_text()) if STATE.exists() else {}
    settings = validate(saved.get('monitors', DEFAULT))
    for _ in range(60):
        try:
            topology()
            time.sleep(3)
            configure(settings)
            break
        except (ValueError, RuntimeError, GLib.Error, subprocess.SubprocessError):
            time.sleep(1)
    else:
        raise RuntimeError('等待 NVIDIA 桌面超时')
    fd = os.open(PORT, os.O_RDWR | os.O_NONBLOCK)
    pending, outgoing = b'', b''
    last_state, next_status = None, 0
    try:
        while True:
            readable, writable, _ = select.select([fd], [fd] if outgoing else [], [], .2)
            replies = []
            if writable:
                try:
                    outgoing = outgoing[os.write(fd, outgoing):]
                except BlockingIOError:
                    pass
            if readable:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    data = b''
                if data:
                    pending += data
                    if len(pending) > 8192:
                        pending = b''
                        replies.append(dict(version=2, type='error', message='请求过长'))
                    while b'\n' in pending:
                        line, pending = pending.split(b'\n', 1)
                        request_id = 0
                        try:
                            request = json.loads(line)
                            if not isinstance(request, dict) or request.get('version') != 2:
                                raise ValueError('协议版本不匹配，请更新客户端')
                            request_id = request.get('request_id', 0)
                            if type(request_id) is not int or not 0 <= request_id <= 2147483647:
                                raise ValueError('请求编号无效')
                            operation = request.get('op')
                            if operation == 'query':
                                result = topology()
                            elif operation == 'configure':
                                result = configure(request.get('monitors'))
                            elif operation == 'resize':
                                number = request.get('id')
                                target = next((m for m in settings if m['id'] == number), None)
                                if target is None or not target['auto_resize']:
                                    raise ValueError('该显示器未启用随窗口调整')
                                changed = [dict(m, width=request.get('width'), height=request.get('height'))
                                           if m['id'] == number else m.copy() for m in settings]
                                result = configure(changed)
                            else:
                                raise ValueError('不支持的显示操作')
                            result['request_id'] = request_id
                            replies.append(result)
                        except (ValueError, RuntimeError, OSError, GLib.Error, subprocess.SubprocessError) as exc:
                            replies.append(dict(version=2, type='error', request_id=request_id, message=str(exc)[:1000]))
                else:
                    time.sleep(.1)
            if time.monotonic() >= next_status:
                try:
                    state = topology()
                    encoded = json.dumps(state, sort_keys=True)
                    if encoded != last_state:
                        replies.append(state)
                        last_state = encoded
                except (ValueError, RuntimeError, GLib.Error, subprocess.SubprocessError):
                    pass
                next_status = time.monotonic() + 1
            for reply in replies:
                outgoing += (json.dumps(reply, ensure_ascii=False) + '\n').encode()
            if len(outgoing) > 32768:
                outgoing = b''  # Reconnecting client queries authoritative state.
                last_state = None
    finally:
        os.close(fd)


if __name__ == '__main__':
    serve()
