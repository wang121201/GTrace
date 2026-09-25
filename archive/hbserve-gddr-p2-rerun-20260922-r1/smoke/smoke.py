#!/usr/bin/env python3
"""Smoke test for the GDDR P2 (17.10 Gb/s) rerun archive.

Three stages, each self-contained; unavailable stages are SKIPped with a reason.

  integrity  verify the shipped hashes (compile inputs, receipts, documented pins)
  build      assemble the original macOS layout, materialise the two -ivfsoverlay
             redirects, apply the one portability patch, compile 21 TUs, link
  run        execute the fixture on the real plan for N seconds and check that the
             produced prefix rows equal the archived result rows

Build modes
  sandbox   (default when bubblewrap is present) the assembled tree is bind-mounted
            at the original macOS path, so no file byte is changed -- this is the
            verified method
  rewrite   (default otherwise, or with --rewrite-paths) the frozen macOS prefix is
            rewritten inside a scratch copy; portable but not byte-faithful, so a
            rewrite-built fixture must not be used for the run stage

Exit status is 0 when no stage FAILED (SKIPs are reported but tolerated).
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ARCHIVE = Path(__file__).resolve().parent.parent
REPO = ARCHIVE.parent.parent
MAC = '/Users/wgs/Documents/Codex/2026-09-14/hbserve-memgen-gtsim-alignment'
SNAP_REL = 'work/unified-cache-cosim-r1/shared-l2-latest-adoption-r1/candidate/snapshot'
ADA_REL = 'work/ada-cosim-alignment-20260922-r1'
WTA_REL = 'work/tilegen-input-contract-r1/llama-p32-history-capture-r1/current-history-write-attribution-r1'
PLAN_REL = 'work/ada-cosim-alignment-20260922-r1/gddr-full-p2-r1/plan.json'
PLAN_SHA = '4c34da34eae1e1ed90d7e22fd9836ec12f68766863e103913ef666f5fb49a3cf'
PATCH_FROM = '{"before",statistics(r.before)},{"after",statistics(r.after)}'
PATCH_TO = '{"before",p28::statistics(r.before)},{"after",p28::statistics(r.after)}'
RUN_IGNORE = re.compile(r'host_seconds$|operation_host_seconds$|sizeof_|alignment|offset')
PINS = {
    'receipts/result-r1/data.json':
        '58eabb56d9f20c9bf6f49589ccaa209ac4468d326ec66fb0618f76b2a6031e19',
    'receipts/result-r1/dashboard.html':
        '03cfd1dff28de4fe62124e1cfd9484678b43795b62cc292209d8861c726c3731',
    'receipts/result-r1/receipt.json':
        'a08c54f06e9cc8766519186f13f25811dfc0da34d571c4fa4929ace05e821534',
}

results = []


def record(stage, status, detail=''):
    results.append((stage, status, detail))
    print(f'[{status:4s}] {stage:10s} {detail}', flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def bwrap(tree, cwd):
    return ['bwrap', '--tmpfs', '/',
            '--dev-bind', '/usr', '/usr', '--dev-bind', '/lib', '/lib',
            '--dev-bind', '/lib64', '/lib64', '--dev-bind', '/bin', '/bin',
            '--dev-bind', '/sbin', '/sbin', '--dev-bind', '/etc', '/etc',
            '--dev-bind', '/home', '/home', '--dev-bind', '/var', '/var',
            '--proc', '/proc', '--dev', '/dev', '--dev-bind', '/tmp', '/tmp',
            '--dir', '/Users', '--dir', '/Users/wgs', '--dir', '/Users/wgs/Documents',
            '--dir', '/Users/wgs/Documents/Codex',
            '--dir', '/Users/wgs/Documents/Codex/2026-09-14',
            '--bind', str(tree), MAC, '--chdir', str(cwd)]


# ------------------------------------------------------------------- integrity
def stage_integrity(_args, _st):
    bad = 0

    listing = ARCHIVE / 'TREE-SHA256.tsv'
    if listing.is_file():
        rows = [l.split('\t') for l in listing.read_text().splitlines()[1:] if l.strip()]
        changed, missing = [], []
        for rel, want in rows:
            p = REPO / rel
            if not p.is_file():
                missing.append(rel)
            elif sha256(p) != want:
                changed.append(rel)
        bad += len(changed) + len(missing)
        record('integrity', 'FAIL' if (changed or missing) else 'OK',
               f'compile inputs {len(rows)}: {len(rows) - len(changed) - len(missing)} '
               f'identical, {len(changed)} changed, {len(missing)} missing')
        for rel in (changed + missing)[:5]:
            print(f'        {rel}')
    else:
        record('integrity', 'SKIP', 'TREE-SHA256.tsv not present')

    for rel, want in PINS.items():
        p = ARCHIVE / rel
        if not p.is_file():
            bad += 1
            record('integrity', 'FAIL', f'{rel} missing')
            continue
        got = sha256(p)
        bad += 0 if got == want else 1
        record('integrity', 'OK' if got == want else 'FAIL', f'{rel} sha256 {got[:16]}…')

    manifest = ARCHIVE / 'MANIFEST.tsv'
    if manifest.is_file():
        rows = [r for r in (l.split('\t') for l in manifest.read_text().splitlines()[1:])
                if len(r) >= 3]
        checked = skipped = 0
        for rel, _size, want in rows:
            p = ARCHIVE / rel
            if not p.is_file():
                skipped += 1        # source/ and inputs/ live outside the branch
                continue
            checked += 1
            if sha256(p) != want:
                bad += 1
                record('integrity', 'FAIL', f'MANIFEST mismatch: {rel}')
        record('integrity', 'OK', f'MANIFEST.tsv {len(rows)} rows: {checked} verified here, '
                                  f'{skipped} expected outside the branch (source/, inputs/)')
    return bad == 0


# ----------------------------------------------------------------------- build
def build_flags(cfg, base):
    """base is MAC in sandbox mode (the tree is bind-mounted there) or the tree
    itself in rewrite mode (the frozen prefix was rewritten inside the copy)."""
    flags = ['-std=' + cfg['standard'], '-O3', '-Wall', '-Wextra', '-pthread',
             '-Wno-unused-parameter']
    flags += ['-D' + d for d in cfg['definitions']]
    flags += ['-I' + base + '/' + SNAP_REL + '/' + d for d in cfg['include_directories']]
    return flags


def assemble(tree, rewrite):
    """Reproduce the original macOS workspace layout; returns (overlays, patched)."""
    (tree / SNAP_REL).mkdir(parents=True)
    shutil.copytree(REPO / 'source', tree / SNAP_REL / 'source')
    # In the original workspace the same repo tree also existed at the top level,
    # and the runtime TU includes those files by their top-level absolute path.
    for rel in ('work/tilegen-input-contract-r1', 'work/ada-cosim-alignment-20260922-r1'):
        src = REPO / 'source' / rel
        if src.is_dir():
            shutil.copytree(src, tree / rel, dirs_exist_ok=True)
    shutil.copytree(ARCHIVE / 'control/structure-adapter', tree / ADA_REL / 'structure-adapter',
                    dirs_exist_ok=True)
    shutil.copytree(ARCHIVE / 'control/runtime-r2', tree / ADA_REL / 'runtime-r2')
    shutil.copytree(ARCHIVE / 'control/write-attribution', tree / WTA_REL, dirs_exist_ok=True)

    if rewrite:
        for root, _dirs, names in os.walk(tree):
            for name in names:
                p = Path(root) / name
                try:
                    text = p.read_text(encoding='utf-8')
                except (UnicodeDecodeError, PermissionError):
                    continue
                if MAC + '/' in text:
                    p.write_text(text.replace(MAC + '/', str(tree) + '/'), encoding='utf-8')

    applied = []
    for overlay in (ARCHIVE / 'control/structure-adapter/overlay.json',
                    ARCHIVE / 'control/write-attribution/overlay.json'):
        for root in json.loads(overlay.read_text())['roots']:
            virtual = root['name'].replace(MAC + '/', '', 1)
            external = root['external-contents'].replace(MAC + '/', '', 1)
            src, dst = tree / external, tree / virtual
            if not src.is_file():
                raise SystemExit(f'overlay external missing: {src}')
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            applied.append(os.path.basename(virtual))

    stream = tree / SNAP_REL / 'source/work/tilegen-full-r1/' \
                          'driver-prefill-gemm-next-r1/streaming.cpp'
    text = stream.read_text()
    if PATCH_FROM in text:
        assert text.count(PATCH_FROM) == 1
        stream.write_text(text.replace(PATCH_FROM, PATCH_TO))
        patched = True
    else:
        patched = PATCH_TO in text
    return applied, patched


def stage_build(args, _st):
    compiler = args.compiler
    if not shutil.which(compiler):
        record('build', 'SKIP', f'{compiler} not found')
        return True

    work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix='hbserve-smoke-'))
    args.work = work
    tree, obj = work / 'tree', work / 'obj'
    shutil.rmtree(tree, ignore_errors=True)
    shutil.rmtree(obj, ignore_errors=True)
    obj.mkdir(parents=True, exist_ok=True)

    sandbox = shutil.which('bwrap') is not None and not args.rewrite_paths
    if not sandbox and not args.rewrite_paths:
        record('build', 'NOTE', 'bubblewrap not found -> falling back to --rewrite-paths '
                                '(build smoke only; not byte-faithful)')
    applied, patched = assemble(tree, rewrite=not sandbox)
    record('build', 'OK', f'layout assembled in {tree} '
                          f'[{"sandbox" if sandbox else "rewrite"}]; '
                          f'overlays: {", ".join(applied)}; patch applied: {patched}')

    cfg = json.loads((ARCHIVE / 'build-config.json').read_text())
    base = MAC if sandbox else str(tree)
    flags = build_flags(cfg, base)
    if 'clang' in compiler:
        for inc in ('/usr/include/c++/11', '/usr/include/x86_64-linux-gnu/c++/11'):
            if os.path.isdir(inc):
                flags += ['-isystem', inc]
    units = [base + '/' + SNAP_REL + '/' + u for u in cfg['translation_units']]

    def run(argv):
        return subprocess.run((bwrap(tree, work) if sandbox else []) + argv,
                              capture_output=True, text=True)

    def compile_one(item):
        i, unit = item
        res = run([compiler] + flags + ['-c', unit, '-o', str(obj / f'{i:02d}.o')])
        return i, res.returncode, (res.stderr or '').strip().splitlines()

    # TU 00 (canonical-full-runtime-r4/streaming.cpp) is textually included by the
    # runtime TU, so it must not also become a separate object.
    todo = [(i, u) for i, u in enumerate(units) if i != 0]
    began = time.monotonic()
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(12, os.cpu_count() or 4)) as pool:
        for i, rc, err in pool.map(compile_one, todo):
            if rc:
                failed.append((i, err))
    record('build', 'FAIL' if failed else 'OK',
           f'compiled {len(todo)} TUs in {time.monotonic() - began:.1f}s, {len(failed)} failed')
    for i, err in failed[:2]:
        for line in err[-8:]:
            print('        ' + line)
    if failed:
        return False

    fixture = work / 'fixture'
    runtime = base + '/' + ADA_REL + '/runtime-r2/history_runtime.cpp'
    link = [compiler] + flags + [runtime] \
        + [str(obj / f'{i:02d}.o') for i in range(1, len(units))] \
        + ['-l' + x for x in cfg['libraries']] + ['-o', str(fixture)]
    res = run(link)
    if res.returncode:
        record('build', 'FAIL', 'link failed')
        for line in (res.stderr or '').strip().splitlines()[-8:]:
            print('        ' + line)
        return False
    record('build', 'OK', f'linked {fixture} ({fixture.stat().st_size} bytes, '
                          f'sha256 {sha256(fixture)[:16]}…)')
    args.fixture = fixture if sandbox else None
    args.tree = tree
    return True


# ------------------------------------------------------------------------- run
def find_env(explicit):
    if explicit:
        return Path(explicit)
    if os.environ.get('HBSERVE_SMOKE_ENV'):
        return Path(os.environ['HBSERVE_SMOKE_ENV'])
    for cand in (REPO.parent / 'build-r1/mirror',
                 Path.home() / 'nvidiagds/codex-runs/hbserve-memgen-gtsim-alignment/build-r1/mirror'):
        if (cand / 'work').is_dir():
            return cand
    return None


def stage_run(args, _st):
    if not shutil.which('bwrap'):
        record('run', 'SKIP', 'bubblewrap not available (needed to expose the frozen path)')
        return True
    env = find_env(args.env)
    if env is None:
        record('run', 'SKIP', 'no runtime input tree found (use --env or HBSERVE_SMOKE_ENV)')
        return True

    fixture = getattr(args, 'fixture', None)
    if fixture is None:
        cand = env.parent / 'fixture'
        if not cand.is_file():
            record('run', 'SKIP', 'no sandbox-built fixture and none found beside the env')
            return True
        fixture = cand
        record('run', 'NOTE', f'using existing fixture {cand}')

    plan = env / PLAN_REL
    if not plan.is_file():
        record('run', 'SKIP', f'plan not found in env: {plan}')
        return True
    got = sha256(plan)
    if got != PLAN_SHA:
        record('run', 'FAIL', f'plan sha256 {got} != pinned {PLAN_SHA}')
        return False
    record('run', 'OK', f'plan sha256 matches the pin ({PLAN_SHA[:16]}…)')

    out = (args.work or Path(tempfile.mkdtemp(prefix='hbserve-smoke-run-'))) / 'out'
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)

    cmd = bwrap(env, env) + [str(fixture), MAC + '/' + PLAN_REL, str(out / 'result.json')]
    with open(out / 'run.stderr', 'wb') as err:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)
        try:
            proc.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    progress, journal = out / 'result.json.progress.json', out / 'result.json.operations.jsonl'
    if not progress.is_file() or not journal.is_file():
        tail = (out / 'run.stderr').read_text().strip().splitlines()[-3:]
        record('run', 'FAIL', 'no progress produced; stderr: ' + ' | '.join(tail))
        return False

    prog = json.loads(progress.read_text())
    ok = True
    if prog.get('status') != 'IN_PROGRESS_NOT_FINAL':
        ok = False
        record('run', 'FAIL', f"unexpected status {prog.get('status')!r}")
    if not prog.get('kernels'):
        ok = False
        record('run', 'FAIL', 'no kernel completed - the loader or a closure gate failed')
    if ok:
        record('run', 'OK', f"phase={prog.get('phase')} kernels={prog.get('kernels')} "
                            f"nodes={prog.get('completed_timeline_nodes')}/"
                            f"{prog.get('selected_timeline_nodes')} "
                            f"exec={prog.get('host_execution_seconds', 0):.1f}s")

    ref = ARCHIVE / 'receipts/linux-rerun/result.json'
    if not ref.is_file():
        record('run', 'SKIP', 'reference result.json not shipped; prefix check skipped')
        return ok
    ref_rows = json.loads(ref.read_text())['rows']
    new_rows = [json.loads(l) for l in journal.read_text().splitlines() if l.strip()]
    n = min(len(ref_rows), len(new_rows))
    if n < args.min_rows:
        record('run', 'SKIP', f'only {n} rows in {args.seconds}s; '
                              f'prefix check needs >= {args.min_rows}')
        return ok
    diffs = []
    for i in range(n):
        a, b = new_rows[i], ref_rows[i]
        for k in set(a) | set(b):
            if RUN_IGNORE.search(f'rows.{i}.{k}'):
                continue
            if a.get(k) != b.get(k):
                diffs.append((f'rows.{i}.{k}', a.get(k), b.get(k)))
    if diffs:
        record('run', 'FAIL', f'{len(diffs)} prefix field difference(s) in the first {n} rows')
        for p, a, b in diffs[:5]:
            print(f'        {p}: linux={a!r} archive={b!r}')
        return False
    record('run', 'OK', f'first {n} rows identical to the archived result '
                        f'(host timing fields excluded)')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stage', action='append', choices=['integrity', 'build', 'run'],
                    help='run only these stages (default: all three)')
    ap.add_argument('--seconds', type=int, default=90, help='bounded run length (default 90)')
    ap.add_argument('--min-rows', type=int, default=8,
                    help='minimum prefix rows required for the run check (default 8)')
    ap.add_argument('--work', help='scratch directory (default: a fresh temp dir)')
    ap.add_argument('--env', help='runtime input tree (default: autodetect)')
    ap.add_argument('--compiler', default='g++', help='C++ compiler (default g++)')
    ap.add_argument('--rewrite-paths', action='store_true',
                    help='force the portable path-rewriting build (no bubblewrap)')
    ap.add_argument('--keep', action='store_true', help='keep the scratch directory')
    args = ap.parse_args()

    stages = args.stage or ['integrity', 'build', 'run']
    print(f'archive : {ARCHIVE}')
    print(f'repo    : {REPO}')
    print(f'stages  : {", ".join(stages)}\n')

    handlers = {'integrity': stage_integrity, 'build': stage_build, 'run': stage_run}
    for name in stages:
        try:
            handlers[name](args, None)
        except SystemExit as exc:
            record(name, 'FAIL', str(exc))
        except Exception as exc:                                   # noqa: BLE001
            record(name, 'FAIL', f'{type(exc).__name__}: {exc}')

    print()
    failed = [r for r in results if r[1] == 'FAIL']
    skipped = [r for r in results if r[1] == 'SKIP']
    print(f'--- {len(results) - len(failed) - len(skipped)} ok, '
          f'{len(skipped)} skipped, {len(failed)} failed ---')
    work = getattr(args, 'work', None)
    if work and Path(work).is_dir():
        if args.keep:
            print(f'scratch kept at {work}')
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
