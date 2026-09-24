"""Exact failed-run retry chains for this finite Qwen Prefill/D2 campaign."""
import hashlib
import json
from pathlib import Path
import re

FIXED_ENV = dict(TILEGEN_ADA_L1_PROFILE='r4', TILEGEN_ADA_REQUIRE_OBSERVED='1',
    TILEGEN_L2_DIRTY_AGE_ACCESSES='64000000', TILEGEN_EF_HIT_RATE='288',
    PYTHONDONTWRITEBYTECODE='1', OMP_NUM_THREADS='1', MKL_NUM_THREADS='1', OPENBLAS_NUM_THREADS='1')
ENV_KEYS = set(FIXED_ENV) | {'TILEGEN_NATIVE_TREE','TILEGEN_NATIVE_SUPPORT_TREE'}

def need(ok, message):
    if not ok: raise ValueError(message)

def pin(path):
    path = Path(path).resolve(); h = hashlib.sha256(); size = 0
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block); size += len(block)
    return dict(path=str(path), bytes=size, sha256=h.hexdigest())

def checked(row):
    need(type(row) is dict and set(row) == {'path','bytes','sha256'} and
         type(row['path']) is str and Path(row['path']).is_absolute() and
         type(row['bytes']) is int and row['bytes'] >= 0 and
         type(row['sha256']) is str and re.fullmatch('[0-9a-f]{64}', row['sha256']), 'retry evidence pin fields')
    need(pin(row['path']) == row, 'retry evidence pin mismatch'); return row

def load(path): return json.loads(Path(path).read_text())

def logical_id(prefill):
    need(type(prefill) is int and prefill in (64,256,512), 'finite retry prefill')
    return 'qwen-p%dd2' % prefill

def retry_number(case_id, prefill):
    base = logical_id(prefill)
    if case_id == base: return 0
    match = re.fullmatch(re.escape(base) + r'-retry-r([1-9][0-9]*)', case_id)
    need(match is not None, 'explicit finite retry case ID required'); return int(match.group(1))

def fixed_environment(env):
    need(set(env) == ENV_KEYS and all(env.get(k) == v for k,v in FIXED_ENV.items()),
         'fixed r4 retry environment changed')

def fixed_spec(prepare):
    rows = prepare['specs']
    need(type(rows) is list and len(rows) == 1 and rows[0]['profile'] == 'r4', 'one fixed r4 retry spec required')
    sp = checked(rows[0]['spec']); spec = load(sp['path'])
    need(spec['case_id'] == prepare['case_id'] + '-r4' and spec['gpu'] is None, 'retry spec case and CPU-only scope')
    argv = spec['argv']
    for flag,key in (('--graph','graph'),('--launch-resources','launch_resources'),('--runner','binary')):
        need(argv.count(flag) == 1 and argv[argv.index(flag)+1] == prepare[key]['path'], 'retry execution spec input changed')
    fixed_environment(spec['environment']); return sp

def edge(parent_path, parent, child):
    proof = child.get('supersedes_failed_case')
    need(type(proof) is dict and set(proof) == {'prepare','job_finish'}, 'explicit failed retry proof required')
    old_pin = checked(proof['prepare']); job_pin = checked(proof['job_finish'])
    need(old_pin == pin(parent_path), 'retry must name exact previous preparation')
    need(job_pin['path'] == str(Path(parent_path).resolve().parent/'r4-job/job-finish.json'), 'retry must name predecessor r4 job finish')
    job = load(job_pin['path'])
    need(type(job.get('status')) is str and job['status'].startswith('FAIL_') and
         type(job['process'].get('returncode')) is int and job['process']['returncode'] != 0 and
         job['process']['cleanup']['owned_descendants_empty'] is True, 'only closed failed and cleaned execution may be superseded')
    need(parent['status'] == 'PASS_PREFILL_R4_PREFLIGHT_NOT_EXECUTED', 'predecessor preparation must have passed')
    for key in ('model_key','prefill_length','decode_steps','profile','input_contract'):
        need(child[key] == parent[key], 'retry input identity changed: ' + key)
    for key in ('graph','launch_resources'):
        need(checked(child[key]) == checked(parent[key]), 'retry source identity changed: ' + key)
    sp = fixed_spec(parent)
    need(checked(job['spec']) == sp and job['case_id'] == parent['case_id'] + '-r4'
         and job['gpu'] is None, 'failure belongs to a different original spec')
    need(retry_number(child['case_id'], child['prefill_length']) == retry_number(parent['case_id'], parent['prefill_length']) + 1,
         'retry IDs must form a consecutive linear chain')
    return [old_pin, job_pin, sp]

def selected_preparations(root):
    """Unique terminal per P; historical failures stay immutable and visible."""
    groups = {}
    for path in (Path(root)/'cases').glob('*/prepare-result.json'):
        try: value = load(path)
        except json.JSONDecodeError: continue
        if value.get('schema') != 'PREFILL_R4_PREPARATION_V1': continue
        p = value.get('prefill_length'); name = value.get('case_id')
        if type(p) is not int or p not in (64,256,512) or value.get('model_key') != 'qwen25_1p5b' or value.get('decode_steps') != 2 or value.get('profile') != 'r4': continue
        need(type(name) is str and path.parent.name == name, 'prepared retry case path mismatch'); retry_number(name,p)
        need(value.get('status') == 'PASS_PREFILL_R4_PREFLIGHT_NOT_EXECUTED', 'prepared retry history requires closed preflight')
        groups.setdefault(p, []).append((path,value))
    selected = {}
    for p, rows in groups.items():
        by_id = {v['case_id']:(path,v) for path,v in rows}; need(len(by_id) == len(rows), 'duplicate prepared case ID')
        roots = [(path,v) for path,v in rows if 'supersedes_failed_case' not in v]
        need(len(roots) == 1 and roots[0][1]['case_id'] == logical_id(p), 'duplicate prepared prefill length without unique retry root')
        next_case = {}
        for path,value in rows:
            if 'supersedes_failed_case' not in value: continue
            proof = value['supersedes_failed_case']
            need(type(proof) is dict and type(proof.get('prepare')) is dict, 'invalid retry link')
            parents = [(pp,pv) for pp,pv in rows if str(pp.resolve()) == proof['prepare'].get('path')]
            need(len(parents) == 1, 'retry predecessor absent from this task')
            pp,pv = parents[0]; edge(pp,pv,value); fixed_spec(value)
            need(pv['case_id'] not in next_case, 'forked retry history'); next_case[pv['case_id']] = value['case_id']
        path,value = roots[0]; seen = {value['case_id']}
        while value['case_id'] in next_case:
            name = next_case[value['case_id']]; need(name not in seen, 'cyclic retry history'); seen.add(name)
            path,value = by_id[name]
        need(len(seen) == len(rows), 'disconnected retry history'); selected[p] = (path,value,len(rows)-1)
    return selected

def validate_new(root, admission, graph_pin, contract, resources_pin, spec):
    selected = selected_preparations(root); p = admission['prefill_length']
    if p not in selected:
        need('supersedes_failed_case' not in admission and admission['case_id'] == logical_id(p), 'retry requires an existing unique failed case')
        return []
    need('supersedes_failed_case' in admission, 'one admitted case per prefill length; explicit failed retry required')
    path,old,_ = selected[p]
    child = dict(case_id=admission['case_id'],model_key=admission['model_key'],prefill_length=p,
        decode_steps=admission['decode_steps'],profile='r4',graph=graph_pin,launch_resources=resources_pin,
        input_contract=contract,supersedes_failed_case=admission['supersedes_failed_case'])
    fixed_environment(spec['environment']); return edge(path,old,child)
