#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch pinned upstream archives, verify SHA256, and apply this repository's patches."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

repo = Path(__file__).resolve().parents[1]
catalog = [entry for entry in json.loads((repo / 'sources.lock.json').read_text()) if 'sha256' in entry]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--component', action='append', choices=[entry['name'] for entry in catalog])
parser.add_argument('--cache-dir', type=Path, help='Use existing upstream archives instead of downloading')
parser.add_argument('--download-only', action='store_true')
args = parser.parse_args()
build = repo / '.build'
downloads = build / 'downloads'
downloads.mkdir(parents=True, exist_ok=True)
for entry in catalog:
    if args.component and entry['name'] not in args.component:
        continue
    archive = downloads / (entry['name'] + '.tar.xz')
    if not archive.exists():
        temporary = archive.with_suffix('.part')
        if args.cache_dir:
            shutil.copyfile(args.cache_dir / archive.name, temporary)
        else:
            with urllib.request.urlopen(entry['url'], timeout=120) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != entry['sha256']:
            raise RuntimeError(f'Archive SHA256 mismatch: {entry["name"]}')
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != entry['sha256']:
        raise RuntimeError(f'Cached archive SHA256 mismatch: {archive}')
    print(f'Verified {archive.name}')
    if args.download_only:
        continue
    source = build / entry['name']
    if source.exists():
        raise RuntimeError(f'Source directory already exists: {source}; use --download-only to verify archives')
    with tarfile.open(archive) as tar:
        for member in tar:
            relative = Path(member.name)
            if relative.is_absolute() or '..' in relative.parts or relative.parts[0] != entry['name']:
                raise RuntimeError(f'Unexpected archive path: {member.name}')
        tar.extractall(build, filter='data')
    subprocess.run(['git', 'init', '-q', str(source)], check=True)
    patch = str(repo / entry['patch'])
    subprocess.run(['git', 'apply', '--check', patch], cwd=source, check=True)
    subprocess.run(['git', 'apply', patch], cwd=source, check=True)
    print(f'Applied {entry["patch"]}')
