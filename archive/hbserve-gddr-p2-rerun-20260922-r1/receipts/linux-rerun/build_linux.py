"""Rebuild the archived GDDR P2 fixture on Linux (xmu) from the frozen archive.

Fidelity rules:
  * every archived file keeps its exact bytes; nothing is path-rewritten;
  * the frozen absolute macOS paths are made to resolve by bind-mounting the
    reconstructed tree at its original location inside a bubblewrap sandbox;
  * the only edits are the two VFS overlay substitutions (which the original
    build also applied, via -ivfsoverlay) and one processor-neutral portability
    fix that the repo itself makes on codex/clang-linux-compat (5211abc).

Reads only from the verified archive; writes only into build-r1/.
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
EXTRA = os.path.join(B, 'extra')
MIRROR = os.path.join(B, 'mirror')
OBJ = os.path.join(B, 'obj')
LOGS = os.path.join(B, 'logs')
MAC = '/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment'
SNAP_REL = 'work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot'
SNAP = os.path.join(MIRROR, SNAP_REL)
SNAP_MAC = MAC + '/' + SNAP_REL
ADA_REL = 'work/ada-cosim-alignment-20260922-r1'
ADA_CTL = 'control/ada-cosim-alignment-20260922-r1'


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def counts():
    return sum(len(f) for _, _, f in os.walk(MIRROR))


def merge(src, dst, label):
    added = kept = 0
    for root, _dirs, names in os.walk(src):
        for n in names:
            s = os.path.join(root, n)
            d = os.path.join(dst, os.path.relpath(s, src))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            if os.path.exists(d):
                kept += 1
                continue
            shutil.copy2(s, d)
            added += 1
    print(f'  merge {label}: added={added} kept={kept}')


# ------------------------------------------------------------------- 1. mirror
# The mirror reproduces the original macOS workspace layout, so the frozen
# absolute paths inside the archived sources and inputs resolve unchanged.
if os.path.isdir(MIRROR):
    shutil.rmtree(MIRROR)
shutil.copytree(os.path.join(R, 'source'), MIRROR, symlinks=True)
print('mirror seed (195-file closure):', counts(), 'files')

merge(os.path.join(EXTRA, 'snapshot-source'), SNAP + '/source',
      'snapshot source (22 TUs + snapshot headers)')
merge(os.path.join(EXTRA, 'structure-adapter'), os.path.join(MIRROR, ADA_REL, 'structure-adapter'),
      'structure-adapter (overlay external #1)')
merge(os.path.join(EXTRA, 'write-attribution'),
      os.path.join(MIRROR, 'work/tilegen-input-contract-r1/llama-p32-history-capture-r1/'
                           'current-history-write-attribution-r1'),
      'write-attribution (overlay external #2)')
merge(os.path.join(R, 'inputs'), MIRROR, 'runtime inputs (22)')
if os.path.isdir(os.path.join(R, 'inputs-extra')):
    merge(os.path.join(R, 'inputs-extra'), MIRROR, 'extra pinned inputs (3)')

# The runtime TU lives at its original location and is used byte-for-byte.
rt_src = os.path.join(R, ADA_CTL, 'runtime-r2/history_runtime.cpp')
rt_dst = os.path.join(MIRROR, ADA_REL, 'runtime-r2/history_runtime.cpp')
os.makedirs(os.path.dirname(rt_dst), exist_ok=True)
shutil.copy2(rt_src, rt_dst)
shutil.copy2(os.path.join(R, ADA_CTL, 'runtime-r2/derivation.json'),
             os.path.join(MIRROR, ADA_REL, 'runtime-r2/derivation.json'))
print('runtime TU placed at its frozen path. mirror files:', counts())
print('runtime TU sha256 (archive == mirror):', sha256(rt_src) == sha256(rt_dst))

# -------------------------------------------------- 2. overlay substitutions
# The original build passed two -ivfsoverlay files. GCC has no VFS overlay, so the
# same three redirects are materialised. For a `type: file` overlay of local files
# this is equivalent.
overlay_doc = []
for name, src in (
        ('structure-adapter', os.path.join(R, ADA_CTL, 'structure-adapter/overlay.json')),
        ('write-attribution',
         os.path.join(R, 'control/current-history-write-attribution-r1/overlay.json'))):
    doc = json.load(open(src, encoding='utf-8'))
    for root in doc['roots']:
        assert root['type'] == 'file', root
        target = root['name'].replace(MAC + '/', MIRROR + '/', 1)
        external = root['external-contents'].replace(MAC + '/', MIRROR + '/', 1)
        assert os.path.isfile(external), external
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(external, target)
        overlay_doc.append({'overlay': name, 'virtual': root['name'],
                            'external': root['external-contents']})
        print(f'  overlay {name}: {root["name"].split("include/")[-1]} <- {root["external-contents"].split("/")[-1]}')

# --------------------------------------------------- 3. portability patch
# Neutral, one line. Apple clang 21 accepted the unqualified name; GCC and clang on
# Linux do not. The repo's `codex/clang-linux-compat` commit 5211abc makes the same
# edit. Nothing else in the archived sources is modified.
patch_path = os.path.join(SNAP, 'source/work/tilegen-full-r1/driver-prefill-gemm-next-r1/streaming.cpp')
PATCH_FROM = '{"before",statistics(r.before)},{"after",statistics(r.after)}'
PATCH_TO = '{"before",p28::statistics(r.before)},{"after",p28::statistics(r.after)}'
text = open(patch_path, encoding='utf-8').read()
assert text.count(PATCH_FROM) == 1
open(patch_path, 'w', encoding='utf-8').write(text.replace(PATCH_FROM, PATCH_TO))
print('portability patch applied: statistics() -> p28::statistics in',
      os.path.relpath(patch_path, MIRROR))
patches = [{'file': os.path.relpath(patch_path, MIRROR), 'from': PATCH_FROM, 'to': PATCH_TO,
            'why': 'unqualified lookup rejected by GCC/clang on Linux; same edit as repo 5211abc',
            'apple_clang_needed_it': False}]

# ------------------------------------------------------- 4. sandbox + flags
def bwrap(args, cwd=B):
    return ['bwrap', '--tmpfs', '/',
            '--dev-bind', '/usr', '/usr', '--dev-bind', '/lib', '/lib',
            '--dev-bind', '/lib64', '/lib64', '--dev-bind', '/bin', '/bin',
            '--dev-bind', '/sbin', '/sbin', '--dev-bind', '/etc', '/etc',
            '--dev-bind', '/home', '/home', '--dev-bind', '/var', '/var',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
            '--dir', '/Users', '--dir', '/Users/wgs', '--dir', '/Users/wgs/Documents',
            '--dir', '/Users/wgs/Documents/Codex', '--dir', '/Users/wgs/Documents/Codex/2026-09-14',
            '--bind', MIRROR, MAC, '--chdir', cwd] + args


BW = ('bwrap --tmpfs / --dev-bind /usr /usr --dev-bind /lib /lib --dev-bind /lib64 /lib64 '
      '--dev-bind /bin /bin --dev-bind /sbin /sbin --dev-bind /etc /etc --dev-bind /home /home '
      '--dev-bind /var /var --proc /proc --dev /dev --tmpfs /tmp '
      '--dir /Users --dir /Users/wgs --dir /Users/wgs/Documents --dir /Users/wgs/Documents/Codex '
      '--dir /Users/wgs/Documents/Codex/2026-09-14 --bind ' + MIRROR + ' ' + MAC)

cfg = json.load(open(os.path.join(B, 'assets/build-config.json')))
compiler = os.environ.get('CXX') or 'g++'
flags = ['-std=' + cfg['standard'], '-O3', '-Wall', '-Wextra', '-pthread', '-Wno-unused-parameter']
flags += ['-D' + d for d in cfg['definitions']]
flags += ['-I' + SNAP_MAC + '/' + d for d in cfg['include_directories']]
units = [SNAP_MAC + '/' + u for u in cfg['translation_units']]
for u in units:
    assert os.path.isfile(u.replace(MAC + '/', MIRROR + '/', 1)), u
print('compiler:', compiler, '| TUs:', len(units))

# ------------------------------------------------------------- 5. compile
os.makedirs(OBJ, exist_ok=True)
os.makedirs(LOGS, exist_ok=True)


def compile_tu(item):
    i, unit = item
    cmd = bwrap([compiler] + flags + ['-c', unit, '-o', os.path.join(OBJ, f'{i:02d}.o')])
    began = time.monotonic()
    res = subprocess.run(cmd, capture_output=True, text=True)
    with open(os.path.join(LOGS, f'{i:02d}.stderr'), 'w') as f:
        f.write(res.stderr or '')
    return i, res.returncode, time.monotonic() - began, (res.stderr or '').strip().splitlines()


# index 00 (canonical-full-runtime-r4/streaming.cpp) is textually included by the
# runtime TU, so it is not a separate object.
todo = [(i, u) for i, u in enumerate(units) if i != 0]
print(f'compiling {len(todo)} TUs inside the sandbox ...')
began = time.monotonic()
results = []
with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
    for r in pool.map(compile_tu, todo):
        results.append(r)
        print(f'  {r[0]:02d} rc={r[1]} {r[2]:6.1f}s' + ('' if r[1] == 0 else '  <- FAILED'))
print(f'compile wall: {time.monotonic() - began:.1f}s')
failed = [r for r in results if r[1] != 0]
if failed:
    for i, rc, secs, err in failed:
        print(f'--- {i:02d} stderr (last 30) ---')
        for line in err[-30:]:
            print('   ', line)
    sys.exit(3)

# ---------------------------------------------------------------- 6. link
runtime_mac = MAC + '/' + ADA_REL + '/runtime-r2/history_runtime.cpp'
link = bwrap([compiler] + flags + [runtime_mac]
             + [os.path.join(OBJ, f'{i:02d}.o') for i in range(1, len(units))]
             + ['-l' + x for x in cfg['libraries']] + ['-o', os.path.join(B, 'fixture')])
res = subprocess.run(link, capture_output=True, text=True)
with open(os.path.join(LOGS, 'link.stderr'), 'w') as f:
    f.write(res.stderr or '')
print('link rc =', res.returncode)
if res.returncode:
    for line in (res.stderr or '').strip().splitlines()[-40:]:
        print('   ', line)
    sys.exit(4)

binary = os.path.join(B, 'fixture')
receipt = {
    'schema': 'HBSERVE_GDDR_P2_LINUX_REBUILD_V1',
    'status': 'PASS_BUILD_ONLY_NO_SIMULATION',
    'compiler': compiler,
    'flags': flags,
    'sandbox': BW,
    'path_fidelity': {
        'bytes_modified_for_paths': 0,
        'mechanism': 'bind-mount the reconstructed workspace at its original macOS path inside bubblewrap',
    },
    'overlay_substitutions': overlay_doc,
    'source_patches': patches,
    'objects': len(todo),
    'binary': {'path': binary, 'bytes': os.path.getsize(binary), 'sha256': sha256(binary)},
    'simulation_executed': False,
}
with open(os.path.join(B, 'build-receipt.json'), 'w') as f:
    json.dump(receipt, f, indent=2)
    f.write('\n')
print('BUILD OK')
print(json.dumps({k: receipt[k] for k in ('status', 'compiler', 'objects', 'binary')}, indent=2))
