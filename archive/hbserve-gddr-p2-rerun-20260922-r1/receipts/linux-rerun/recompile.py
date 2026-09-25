"""Re-apply the two build-time adjustments and recompile against the completed mirror.

The mirror is no longer wiped (it now holds the bulk-fetched provenance). This
re-asserts the VFS-overlay substitutions and the single portability patch, then
rebuilds the fixture inside the same bubblewrap sandbox.
"""
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

R = '/home/xmu/nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment'
B = os.path.join(R, 'build-r1')
MIRROR = os.path.join(B, 'mirror')
OBJ = os.path.join(B, 'obj')
LOGS = os.path.join(B, 'logs')
MAC = '/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment'
SNAP_REL = 'work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot'
ADA_CTL = 'control/ada-cosim-alignment-20260922-r1'
INNER = 'work/tilegen-full-r1/core-native-copy-r2/include'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------- 1. overlay substitutions
subs = []
for name, src in (
        ('structure-adapter', os.path.join(R, ADA_CTL, 'structure-adapter/overlay.json')),
        ('write-attribution', os.path.join(R, 'control/current-history-write-attribution-r1/overlay.json'))):
    for root in json.load(open(src, encoding='utf-8'))['roots']:
        target = root['name'].replace(MAC + '/', MIRROR + '/', 1)
        external = root['external-contents'].replace(MAC + '/', MIRROR + '/', 1)
        assert os.path.isfile(external), external
        with open(external, 'rb') as f:
            want = hashlib.sha256(f.read()).hexdigest()
        have = sha256(target) if os.path.isfile(target) else None
        if have != want:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy2(external, target)
            print(f'  re-applied overlay {name}: {os.path.basename(target)}')
        else:
            print(f'  overlay {name} already applied: {os.path.basename(target)}')
        subs.append({'overlay': name, 'virtual': root['name'], 'external': root['external-contents'],
                     'sha256': want})

# ---------------------------------------------------- 2. portability patch
patch_path = os.path.join(MIRROR, SNAP_REL,
                          'source/work/tilegen-full-r1/driver-prefill-gemm-next-r1/streaming.cpp')
PATCH_FROM = '{"before",statistics(r.before)},{"after",statistics(r.after)}'
PATCH_TO = '{"before",p28::statistics(r.before)},{"after",p28::statistics(r.after)}'
text = open(patch_path, encoding='utf-8').read()
if PATCH_FROM in text:
    assert text.count(PATCH_FROM) == 1
    open(patch_path, 'w', encoding='utf-8').write(text.replace(PATCH_FROM, PATCH_TO))
    print('  portability patch re-applied')
elif PATCH_TO in text:
    print('  portability patch already present')
else:
    print('  !! neither patched nor unpatched text found'); sys.exit(2)

# ------------------------------------------------------ 3. sandbox + flags
BW = ['bwrap', '--tmpfs', '/',
      '--dev-bind', '/usr', '/usr', '--dev-bind', '/lib', '/lib',
      '--dev-bind', '/lib64', '/lib64', '--dev-bind', '/bin', '/bin',
      '--dev-bind', '/sbin', '/sbin', '--dev-bind', '/etc', '/etc',
      '--dev-bind', '/home', '/home', '--dev-bind', '/var', '/var',
      '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
      '--dir', '/Users', '--dir', '/Users/wgs', '--dir', '/Users/wgs/Documents',
      '--dir', '/Users/wgs/Documents/Codex', '--dir', '/Users/wgs/Documents/Codex/2026-09-14',
      '--bind', MIRROR, MAC, '--chdir', B]

cfg = json.load(open(os.path.join(B, 'assets/build-config.json')))
compiler = os.environ.get('CXX') or 'g++'
flags = ['-std=' + cfg['standard'], '-O3', '-Wall', '-Wextra', '-pthread', '-Wno-unused-parameter']
flags += ['-D' + d for d in cfg['definitions']]
flags += ['-I' + MAC + '/' + SNAP_REL + '/' + d for d in cfg['include_directories']]
units = [MAC + '/' + SNAP_REL + '/' + u for u in cfg['translation_units']]

os.makedirs(OBJ, exist_ok=True)
os.makedirs(LOGS, exist_ok=True)


def compile_tu(item):
    i, unit = item
    cmd = BW + [compiler] + flags + ['-c', unit, '-o', os.path.join(OBJ, f'{i:02d}.o')]
    res = subprocess.run(cmd, capture_output=True, text=True)
    with open(os.path.join(LOGS, f'{i:02d}.stderr'), 'w') as f:
        f.write(res.stderr or '')
    return i, res.returncode, (res.stderr or '').strip().splitlines()


todo = [(i, u) for i, u in enumerate(units) if i != 0]
print(f'recompiling {len(todo)} TUs ...')
began = time.monotonic()
bad = []
with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
    for i, rc, err in pool.map(compile_tu, todo):
        if rc:
            bad.append((i, err))
print(f'compile wall: {time.monotonic() - began:.1f}s, failures: {len(bad)}')
for i, err in bad:
    print(f'--- {i:02d} ---')
    for line in err[-20:]:
        print('   ', line)
if bad:
    sys.exit(3)

link = BW + [compiler] + flags + [MAC + '/' + ADA_CTL.replace('control/', 'work/') + '/runtime-r2/history_runtime.cpp'] \
       + [os.path.join(OBJ, f'{i:02d}.o') for i in range(1, len(units))] \
       + ['-l' + x for x in cfg['libraries']] + ['-o', os.path.join(B, 'fixture')]
res = subprocess.run(link, capture_output=True, text=True)
with open(os.path.join(LOGS, 'link.stderr'), 'w') as f:
    f.write(res.stderr or '')
print('link rc =', res.returncode)
if res.returncode:
    for line in (res.stderr or '').strip().splitlines()[-30:]:
        print('   ', line)
    sys.exit(4)

binary = os.path.join(B, 'fixture')
receipt = json.load(open(os.path.join(B, 'build-receipt.json')))
receipt['overlay_substitutions'] = subs
receipt['binary'] = {'path': binary, 'bytes': os.path.getsize(binary), 'sha256': sha256(binary)}
receipt['rebuild_note'] = 'recompiled after the bulk provenance fetch restored pristine upstream bytes'
with open(os.path.join(B, 'build-receipt.json'), 'w') as f:
    json.dump(receipt, f, indent=2)
    f.write('\n')
print('BUILD OK ->', json.dumps(receipt['binary']))
print('mirror files:', sum(len(f) for _, _, f in os.walk(MIRROR)))
