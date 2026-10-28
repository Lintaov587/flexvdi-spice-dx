#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Collect a fresh portable Windows runtime and resolve its PE dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--msys-root', type=Path, required=True)
parser.add_argument('--build-root', type=Path, default=repo / '.build')
parser.add_argument('--output', type=Path, default=repo / 'dist/flexvdi-spice-dx')
parser.add_argument('--usb-ids', type=Path, help='Optional upstream usb.ids database for readable USB names')
args = parser.parse_args()
ucrt = args.msys_root.resolve() / 'ucrt64'
build = args.build_root.resolve()
dest = args.output.resolve()
if dest.exists():
    raise RuntimeError(f'Choose a new output directory: {dest}')
dest.mkdir(parents=True)
windows = Path(os.environ['SystemRoot']) / 'System32'
search = [build / 'build-flexvdi/src', build / 'build-spice/src', build / 'flexvdi-prefix/bin', ucrt / 'bin']
pending, visited, system = [], set(), set()

def copy(source, relative):
    target = dest / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    if target.suffix.lower() in {'.exe', '.dll'}:
        pending.append(target)

def tree(source, relative):
    if not source.is_dir():
        raise RuntimeError(f'Missing runtime directory: {source}')
    for path in source.rglob('*'):
        if path.is_file() and path.name not in {'libgstsoup.dll', 'libgstadaptivedemux2.dll', 'loaders.cache'}:
            copy(path, Path(relative) / path.relative_to(source))

copy(build / 'build-flexvdi/src/flexvdi-client.exe', 'bin/flexvdi-client.exe')
for relative in ('lib/gio/modules', 'libexec/gstreamer-1.0',
                 'lib/gdk-pixbuf-2.0', 'share/glib-2.0/schemas', 'share/icons/Adwaita',
                 'share/icons/hicolor', 'etc/fonts', 'share/licenses'):
    tree(ucrt / relative, relative)
for name in json.loads((repo / 'tools/windows/gstreamer-plugins.json').read_text()):
    copy(ucrt / 'lib/gstreamer-1.0' / name, 'lib/gstreamer-1.0/' + name)
for name in ('gdk-pixbuf-query-loaders.exe', 'gst-inspect-1.0.exe'):
    copy(ucrt / 'bin' / name, 'bin/' + name)
copy(repo / 'launcher/Launch.ps1', 'Launch.ps1')
copy(repo / 'LICENSE', 'LICENSE-flexvdi.txt')
tree(repo / 'licenses', 'licenses')
copy(repo / 'sources.lock.json', 'sources.lock.json')
if args.usb_ids:
    copy(args.usb_ids, 'share/hwdata/usb.ids')

while pending:
    binary = pending.pop()
    if binary in visited:
        continue
    visited.add(binary)
    output = subprocess.check_output([str(ucrt / 'bin/objdump.exe'), '-p', str(binary)], text=True)
    for name in re.findall(r'DLL Name:\s*(\S+)', output):
        if name.lower().startswith('libsoup-3'):
            raise RuntimeError(f'libsoup 3 must not coexist with libsoup 2: {binary}')
        target = dest / 'bin' / name
        if target.exists():
            continue
        found = next((folder / name for folder in search if (folder / name).is_file()), None)
        if found:
            copy(found, 'bin/' + name)
        elif (windows / name).is_file() or name.lower().startswith(('api-ms-', 'ext-ms-')):
            system.add(name)
        else:
            raise RuntimeError(f'Unresolved dependency {name} required by {binary}')

for name in ('libgstd3d11.dll', 'libgstvideoparsersbad.dll'):
    if not (dest / 'lib/gstreamer-1.0' / name).is_file():
        raise RuntimeError(f'Missing required GStreamer plugin: {name}')
manifest = dict(windows_system_dependencies=sorted(system), binaries_sha256={
    str(path.relative_to(dest)).replace('\\', '/'): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted(visited)})
(dest / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(f'Packaged {len(visited)} PE binaries in {dest}')
