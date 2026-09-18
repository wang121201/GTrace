#!/usr/bin/env python3
"""Freeze the effective non-B8 compiler dependency closure without changing it."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

HERE = Path(__file__).resolve().parent
REPO = HERE / 'repo'
ORIGIN = Path('/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def normal(path):
    return str(Path(os.path.normpath(path)))


def relative(path):
    return Path('source') / Path(normal(path)).relative_to(ORIGIN)


def main():
    if REPO.exists():
        raise ValueError('fresh repository destination required')
    inp = json.loads((HERE / 'import-build-input.json').read_text())
    flags = inp['flags']
    overlay_path = Path(flags[flags.index('-ivfsoverlay') + 1])
    overlay_bytes = overlay_path.read_bytes()
    overlay = json.loads(overlay_bytes)
    aliases = {}
    for row in overlay['roots']:
        assert row['type'] == 'file'
        key = normal(row['name'])
        assert key not in aliases
        aliases[key] = normal(row['external-contents'])
    logs = HERE / 'dependency-closure'
    logs.mkdir(exist_ok=False)
    started = time.monotonic()

    def scan(item):
        i, tu = item
        cmd = [inp['compiler'], *flags, '-MM', '-MT', 'dependency_target', tu]
        result = subprocess.run(cmd, capture_output=True, timeout=120)
        (logs / f'{i:02d}.d').write_bytes(result.stdout)
        (logs / f'{i:02d}.stderr').write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError(f'dependency scan failed for {tu}: {result.stderr.decode()}')
        line = result.stdout.decode().replace('\\\n', ' ')
        dependencies = [normal(p) for p in shlex.split(line.split(':', 1)[1])]
        return dict(translation_unit=normal(tu), dependencies=dependencies,
                    command=cmd, seconds=None, returncode=result.returncode)

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        scans = list(pool.map(scan, enumerate(inp['translation_units'])))
    dependencies = sorted(set(p for s in scans for p in s['dependencies']))
    REPO.mkdir()
    rows = []
    for logical in dependencies:
        effective = aliases.get(logical, logical)
        original = Path(effective).read_bytes()
        text = original.decode('utf-8')
        output = REPO / relative(logical)
        rewrites = []

        def include(match):
            target = normal(match.group(2))
            if not target.startswith(str(ORIGIN) + '/'):
                raise ValueError('unmapped absolute include: ' + target)
            if target not in dependencies:
                raise ValueError('absolute include missing from dependency closure: ' + target)
            relocated = os.path.relpath(REPO / relative(target), output.parent)
            rewrites.append(dict(original=match.group(2), replacement=relocated))
            return match.group(1) + relocated + match.group(3)

        text = re.sub(r'(^\s*#\s*include\s*["<])(/[^">]+)([">])', include, text, flags=re.M)
        copied = text.encode('utf-8')
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(copied)
        rows.append(dict(logical_path=logical, effective_path=effective,
                         repository_path=str(relative(logical)), overlay_applied=effective != logical,
                         original_bytes=len(original), original_sha256=digest(original),
                         copied_bytes=len(copied), copied_sha256=digest(copied),
                         absolute_include_rewrites=rewrites))
    config = dict(schema='TILEGEN_NATIVE_BUILD_CONFIG_V1', compiler=inp['compiler'],
                  standard='c++20', definitions=['TILEGEN_DIRTY_SECTOR_MODE=2'],
                  include_directories=[str(relative(f[2:])) for f in flags if f.startswith('-I')],
                  translation_units=[str(relative(t)) for t in inp['translation_units']],
                  libraries=['z'], executable='tilegen_native')
    provenance = REPO / 'provenance'
    provenance.mkdir()
    (REPO / 'build-config.json').write_text(json.dumps(config, indent=2) + '\n')
    (provenance / 'import-build-input.json').write_bytes((HERE / 'import-build-input.json').read_bytes())
    (provenance / 'original-overlay.json').write_bytes(overlay_bytes)
    source_map = dict(schema='TILEGEN_NATIVE_DEPENDENCY_IMPORT_V1',
        input_sha256=digest((HERE / 'import-build-input.json').read_bytes()),
        overlay_sha256=digest(overlay_bytes), overlay_alias_count=len(aliases),
        translation_unit_count=len(scans), file_count=len(rows),
        effective_overlay_files=sum(r['overlay_applied'] for r in rows),
        bytes=sum(r['copied_bytes'] for r in rows),
        native_capture_performed=False, B8_imported=False,
        source_semantics_modified=False, absolute_include_rewrites=sum(len(r['absolute_include_rewrites']) for r in rows),
        source_root=str(ORIGIN), rows=rows)
    (provenance / 'source-map.json').write_text(json.dumps(source_map, indent=2) + '\n')
    (provenance / 'dependency-scans.json').write_text(json.dumps(scans, indent=2) + '\n')
    (provenance / 'import-script.py').write_bytes(Path(__file__).read_bytes())
    print(json.dumps(dict(status='SOURCE_IMPORTED', repository=str(REPO),
                         files=len(rows), bytes=source_map['bytes'],
                         overlay_files=source_map['effective_overlay_files'],
                         rewrites=source_map['absolute_include_rewrites'],
                         wall_seconds=time.monotonic()-started)))


if __name__ == '__main__':
    main()
