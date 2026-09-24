#!/usr/bin/env python3
"""Bounded clock configuration tests using actual compiled runner; no model/GPU."""
import argparse, ast, hashlib, json, os, pathlib, subprocess, time

def main():
    p=argparse.ArgumentParser();p.add_argument('--binary',required=True,type=pathlib.Path);p.add_argument('--input',required=True,type=pathlib.Path);p.add_argument('--output',required=True,type=pathlib.Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    source=pathlib.Path(__file__).resolve().parents[1]/'executor-r1/whole_stream.py'
    names={'need','parse_dirty_age_accesses','dirty_age_expectation','validate_cache_configuration'}
    ns={'argparse':argparse};tree=ast.parse(source.read_text());exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in names],type_ignores=[]),str(source),'exec'),ns)
    receipts=[]
    def run(name,clock,budget='3',success=True,commands=None):
        d=a.output/name;d.mkdir();env=dict(os.environ);env.pop('TILEGEN_L2_DIRTY_AGE_CLOCK',None)
        env.update(TILEGEN_ADA_L1_PROFILE='r4',TILEGEN_ADA_REQUIRE_OBSERVED='1',TILEGEN_L2_DATA_POLICY='sector32',TILEGEN_EF_HIT_RATE='288',TILEGEN_L2_DIRTY_AGE_ACCESSES=budget,ASAN_OPTIONS='detect_leaks=0')
        if clock is not None:env['TILEGEN_L2_DIRTY_AGE_CLOCK']=clock
        input_path=a.input.resolve()
        if commands is not None:
            input_path=(d/'input.jsonl').resolve();input_path.write_text(''.join(json.dumps(x)+'\n' for x in commands))
        started=time.monotonic();r=subprocess.run([str(a.binary.resolve()),'--input',str(input_path),'--summary',str((d/'summary.json').resolve()),'--snapshots',str((d/'snapshots.jsonl').resolve())],env=env,text=True,capture_output=True,timeout=60)
        (d/'stdout').write_text(r.stdout);(d/'stderr').write_text(r.stderr);assert (r.returncode==0)==success,(name,r.returncode,r.stderr)
        receipts.append(dict(name=name,returncode=r.returncode,expected_success=success,wall_seconds=time.monotonic()-started))
        if success:return json.loads((d/'summary.json').read_text())
    default=run('default-global',None);global_=run('explicit-global','global');set_=run('explicit-set','set');disabled=run('set-disabled','set','0')
    assert default['snapshot']==global_['snapshot']
    assert default['dirty_age_observation']==global_['dirty_age_observation']
    for s,clock,budget in ((global_,'global',3),(set_,'set',3),(disabled,'set',0)):
        cfg=s['configuration'];ns['validate_cache_configuration'](cfg,budget,clock)
        assert cfg['L2']['dirty_age_clock']==clock
        assert cfg['L2']['dirty_age_budget_unit']==('GLOBAL_FORWARDED_L2_128B_LINE_ACCESSES' if clock=='global' else 'SAME_GROUP_FORWARDED_L2_128B_LINE_ACCESSES')
        obs=s['dirty_age_observation'];assert obs['selected_clock']==clock
        if clock=='set':assert sum(obs['group_ticks'])==obs['now']==s['snapshot']['L2_forwarded_access_sequence']
        for badclock,badbudget in ((('set' if clock=='global' else 'global'),budget),(clock,budget+1)):
            try:ns['validate_cache_configuration'](cfg,badbudget,badclock)
            except ValueError:pass
            else:raise AssertionError('wrong clock/budget admitted')
    initial=[json.loads(x) for x in a.input.read_text().splitlines()][:2]
    def api_start(i,phase):return dict(type='begin_api',id=i,phase=phase,semantic='API_history',api='synthetic_D2D',dma_model='L2_COHERENT_FUNCTIONAL_128B_CHUNKS')
    api_rows=initial+[dict(type='snapshot',label='Measured/Prefill/begin'),api_start(1,'Measured/Prefill'),dict(type='api_range',operation='WRITE',address=4216,bytes=16),dict(type='end_api',id=1),dict(type='snapshot',label='Measured/Prefill/end'),dict(type='snapshot',label='Measured/Decode1/begin'),api_start(2,'Measured/Decode1')]+[dict(type='api_range',operation='READ',address=4216,bytes=16)]*3+[dict(type='end_api',id=2),dict(type='snapshot',label='Measured/Decode1/end'),dict(type='drain',label='diagnostic',phase='Diagnostic/run-end'),dict(type='run_end')]
    api=run('set-api-cross-line-phases','set',commands=api_rows)
    s=api['snapshot'];assert s['API_128B_chunks']==8 and s['L2_forwarded_access_sequence']==8 and s['DRAM_read_bytes']==64 and s['age_writeback_bytes']==64 and s['DRAM_write_bytes']==64 and s['dirty_tail_bytes']==0
    snapshots={x['label']:x['cumulative'] for x in map(json.loads,(a.output/'set-api-cross-line-phases/snapshots.jsonl').read_text().splitlines()) if x['type']=='snapshot'}
    for phase in ('Measured/Prefill','Measured/Decode1'):
        b,e=snapshots[phase+'/begin'],snapshots[phase+'/end']
        assert b['dirty_tail_bytes']+32*(e['dirty_sector_creations']-b['dirty_sector_creations'])==e['DRAM_write_bytes']-b['DRAM_write_bytes']+e['dirty_tail_bytes']
    assert snapshots['Measured/Prefill/end']['dirty_tail_bytes']==64
    owners=api['dirty_ownership']['writebacks_cumulative'];assert sum(x['write_bytes'] for x in owners)==64 and all(x['last_writer_phase']=='Measured/Prefill' and x['trigger_phase']=='Measured/Decode1' for x in owners)
    for clock in ('','GLOBAL','set_local'):run('invalid-clock-'+str(len(receipts)),clock,success=False)
    for budget in ('-1','18446744073709551616','1.0'):run('invalid-budget-'+str(len(receipts)),'set',budget,False)
    for value in ('-1','18446744073709551616','1.0',''):
        try:ns['parse_dirty_age_accesses'](value)
        except argparse.ArgumentTypeError:pass
        else:raise AssertionError('invalid expected budget accepted')
    assert ns['dirty_age_expectation'](None)['expected_dirty_age_clock']=='global'
    assert ns['dirty_age_expectation'](3125,'set')['expected_dirty_age_clock']=='set'
    try:ns['dirty_age_expectation'](None,'set')
    except ValueError:pass
    else:raise AssertionError('set clock silently reused global default budget')
    result=dict(status='PASS_SET_AGE_CONFIG_CONTRACT',actual_runner_cases=len(receipts),cases=receipts,full_model_or_GPU_test=False,binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),whole_stream_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
    (a.output/'receipt.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
