import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

HERE=Path(__file__).resolve().parent
TOOLS=HERE.parents[1]/'tools'
def load(name):
    spec=importlib.util.spec_from_file_location(name,TOOLS/(name+'.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
P=load('p128_prepare_xmu');S=load('p128_safe_status_xmu')

def save(p,x):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(x));return p

def fixture(root,d=2):
    native=root/'native';support=root/'support';native.mkdir();support.mkdir()
    registry=save(root/'registry.json',{});runtime=native/'runtime.py';runtime.write_text('# synthetic source only\n')
    phase_names=P.phases(d);nodes=root/'nodes.jsonl'
    node_rows=[dict(kind='native_kernel',native_launch_id=i) for i in range(len(phase_names)+1)]
    node_rows += [dict(kind='memory_api_submission'),dict(kind='allocation_API_observation')]
    nodes.write_text(''.join(json.dumps(n)+'\n' for n in node_rows))
    contract=dict(model_key='qwen25_1p5b',prefill_length=128,decode_steps=d,batch_size=1,dtype='bfloat16',warmup_runs=1,sampling_retained=True,output_feedback=False,cuda_graph=False)
    graph=save(root/'graph.json',dict(input_contract=contract,phases=[dict(phase=p) for p in phase_names],artifacts={'nodes':P.pin(nodes)}))
    resources=save(root/'resources.json',dict(schema='OBSERVED_LAUNCH_CARVEOUT_V1',evidence_scope='per_kernel_launch',graph_sha256=P.pin(graph)['sha256'],kernels=[dict(native_launch_id=i,observed_shared_bytes=[8192,16384,32768,65536,102400][i%5]) for i in range(len(phase_names)+1)]))
    admission=dict(schema='ADMITTED_P128_CASE_V1',case_id='qwen-p128d'+str(d),model_key='qwen25_1p5b',prefill_length=128,decode_steps=d,graph=str(graph),registry=str(registry),runtime=str(runtime),native_tree=str(native),support_tree=str(support),launch_resources=str(resources),source_pins=[P.pin(v) for v in (graph,nodes,registry,runtime)],expected_counts=dict(kernels=len(phase_names)+1,memory_APIs=1,allocation_observations=1,phases=len(phase_names)))
    return admission

class PrepareTests(unittest.TestCase):
    def test_four_lengths_complete_source_counts(self):
        for d in (2,4,8,16):
            with self.subTest(decode=d),tempfile.TemporaryDirectory() as tmp:
                a=fixture(Path(tmp).resolve(),d);paths,pins,counts=P.validate_admission(a)
                self.assertEqual(counts['phases'],2*(d+1));self.assertEqual(paths['support_tree'],Path(tmp).resolve()/'support')

    def test_source_mutation_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            a=fixture(Path(tmp).resolve());Path(a['runtime']).write_text('changed')
            with self.assertRaisesRegex(ValueError,'pin mismatch'):P.validate_admission(a)

    def test_resource_missing_extra_duplicate_mismatch_rejected(self):
        for change in ['missing','extra','duplicate','wrong_graph','range_scope']:
            with self.subTest(change=change),tempfile.TemporaryDirectory() as tmp:
                a=fixture(Path(tmp).resolve());p=Path(a['launch_resources']);x=json.loads(p.read_text())
                if change=='missing':x['kernels'].pop(0)
                elif change=='extra':x['kernels'].append(dict(native_launch_id=999,observed_shared_bytes=32768))
                elif change=='duplicate':x['kernels'].append(x['kernels'][0])
                elif change=='wrong_graph':x['graph_sha256']='0'*64
                else:x['evidence_scope']='ROI'
                save(p,x)
                with self.assertRaises(ValueError):P.validate_admission(a)

    def test_counts_contract_path_and_budget_reject(self):
        for key,value in [('decode_steps',3),('prefill_length',32),('case_id','../escape'),('graph','relative.json')]:
            with self.subTest(key=key),tempfile.TemporaryDirectory() as tmp:
                a=fixture(Path(tmp).resolve());a[key]=value
                with self.assertRaises(ValueError):P.validate_admission(a)
        with tempfile.TemporaryDirectory() as tmp:
            a=fixture(Path(tmp).resolve());a['expected_counts']['kernels']+=1
            with self.assertRaisesRegex(ValueError,'actual graph counts'):P.validate_admission(a)

    def test_specs_fixed_binary_cache_two_distinct_single_cpus(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();a=fixture(root);paths,pins,_=P.validate_admission(a)
            _,_,_,specs=P.build_specs(root,a,paths,(4,7),pins)
            for profile,spec in specs.items():
                self.assertIsNone(spec['gpu']);self.assertEqual(spec['seconds'],43200);self.assertEqual(spec['rss_limit_bytes'],16<<30)
                self.assertEqual(spec['argv'][spec['argv'].index('--runner')+1],str(P.BINARY))
                self.assertIn('--fast-gemv',spec['argv']);self.assertIn('--fast-prefill',spec['argv']);self.assertNotIn('--fast-down',spec['argv'])
                self.assertEqual(spec['environment']['TILEGEN_ADA_REQUIRE_OBSERVED'],'1');self.assertEqual(spec['environment']['TILEGEN_EF_HIT_RATE'],'288')
                self.assertEqual(spec['environment']['TILEGEN_L2_DIRTY_AGE_ACCESSES'],'64000000')
                self.assertEqual(spec['environment']['TILEGEN_NATIVE_SUPPORT_TREE'],a['support_tree'])
            for cpus in [(4,4),(-1,2),(2,16)]:
                with self.assertRaises(ValueError):P.build_specs(root,a,paths,cpus,pins)

    def test_frozen_binary_and_critical_source_pins(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();binary=root/'build-qwen/source-cache-runner';binary.parent.mkdir();binary.write_bytes(b'\x7fELFfixture')
            paths=[binary]
            for name in P.KEY_BINARY_SOURCES:
                p=root/'repo'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('// synthetic '+name);paths.append(p)
            spec=save(root/'qwen-r2-spec.json',dict(sources=[P.pin(p) for p in paths]))
            with patch.object(P,'OLD_ROOT',root),patch.object(P,'BINARY',binary),patch.object(P,'BINARY_SPEC',spec):
                self.assertEqual(len(P.binary_evidence()),len(paths)+1)
                binary.write_bytes(b'\x7fELFchanged')
                with self.assertRaisesRegex(ValueError,'pin mismatch'):P.binary_evidence()

    def test_prepare_only_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();inputs=root/'inputs';inputs.mkdir();a=fixture(inputs);admission=save(root/'admission.json',a)
            producer=root/'repo/llm/executor-r1/whole_stream.py';producer.parent.mkdir(parents=True);producer.write_text('# fixture no execution')
            fake_binary=root/'binary';fake_binary.write_bytes(b'\x7fELFfixture')
            def preflight(cmd,**kwargs):
                self.assertIn('--preflight-only',cmd);self.assertNotEqual(cmd[0],str(fake_binary))
                self.assertEqual(kwargs['env']['TILEGEN_NATIVE_SUPPORT_TREE'],a['support_tree'])
                out=Path(cmd[cmd.index('--output')+1]);save(out/'status.json',dict(status='PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT',input_preflight=dict(kernel_count=a['expected_counts']['kernels'],phases=P.phases(2)),fast_source_runtime_pins=[]))
                return types.SimpleNamespace(stdout='',stderr='',check_returncode=lambda:None)
            argv=['prepare','--root',str(root),'--admission',str(admission),'--cpu-r2','2','--cpu-r4','3']
            with patch.object(P,'REMOTE_ROOT',root),patch.object(P,'BINARY',fake_binary),patch.object(P,'binary_evidence',return_value=[P.pin(fake_binary)]),patch.object(P.subprocess,'run',side_effect=preflight) as run,patch.object(sys,'argv',argv),contextlib.redirect_stdout(io.StringIO()):
                P.main();self.assertEqual(run.call_count,1)
                with self.assertRaises(ValueError):P.main()
            result=json.loads((root/'cases'/a['case_id']/'prepare-result.json').read_text())
            self.assertFalse(result['compiled']);self.assertFalse(result['launched']);self.assertEqual(len(result['specs']),2)

class StatusTests(unittest.TestCase):
    def test_pending_all_eight_and_no_fabricated_ncu(self):
        with tempfile.TemporaryDirectory() as tmp:
            x=S.aggregate(Path(tmp).resolve());self.assertEqual(len(x['cases']),8)
            self.assertTrue(all(c['status']=='NOT_STARTED' and c['NCU']['ROI_rows'] is None for c in x['cases']))

    def test_aggregate_whitelist_and_phase_deltas(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();directory=root/'cases/qwen-p128d2';out=directory/'r4'
            secret='DO_NOT_EXPORT_KERNEL_SYMBOL_0xdeadbeef'
            save(directory/'prepare-result.json',dict(schema='P128_PAIRED_PREPARATION_V1',case_id='qwen-p128d2',model_key='qwen25_1p5b',prefill_length=128,decode_steps=2))
            save(out/'status.json',dict(status='RUNNING_COMPLETE_NATIVE_GRAPH_CACHE',error=secret,error_type='ValueError',executed_counts={'kernels':7,secret:5},source_stream=dict(bytes=42,records=4,sha256='a'*64,raw_address=secret)))
            save(out/'cache-summary.json',dict(CPU_minutes=1.5,configuration={'L1':{'profile':'r4','hash':'ALLOCATION_RELATIVE_HASH2','bytes_per_SM':102400,'symbol':secret},'L2':{'bytes':41943040,'geometry':'PAPER_ADA_L2_V1_20x1024x16','address':secret}},snapshot={'DRAM_read_bytes':128,'raw_address':123456,'L1_adapter_observation':{'L1_covered_min_line':987654},'native_symbol':secret}))
            snapshots=[dict(type='snapshot',label='Measured/Decode1/begin',CPU_minutes_since_run_start=1,cumulative=dict(DRAM_read_bytes=128,DRAM_write_bytes=0,dirty_tail_bytes=32)),dict(type='snapshot',label='Measured/Decode1/end',CPU_minutes_since_run_start=2,cumulative=dict(DRAM_read_bytes=384,DRAM_write_bytes=32,dirty_tail_bytes=0))]
            (out/'cache-snapshots.jsonl').write_text(''.join(json.dumps(s)+'\n' for s in snapshots))
            x=S.aggregate(root);serialized=json.dumps(x);self.assertNotIn(secret,serialized);self.assertNotIn('987654',serialized);self.assertNotIn('123456',serialized)
            c=next(c for c in x['cases'] if c['decode_steps']==2 and c['profile']=='r4');self.assertEqual(c['completed_phases'][0]['DRAM_read_bytes'],256);self.assertEqual(c['completed_phases'][0]['cache_CPU_minutes'],1)

    def test_ncu_present_with_duration_string_and_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory=Path(tmp).resolve()
            contract=dict(model_key='qwen25_1p5b',prefill_length=128,decode_steps=4,batch_size=1,dtype='bfloat16',warmup_runs=1,prompt_ids=[1,2],decode_input_ids=[3,4,3,4],sampling_retained=True,output_feedback=False,cuda_graph=False)
            save(directory/'prepare-result.json',dict(input_contract=contract))
            row=dict(roi='D4',n=1,dram_read_bytes=100,dram_write_bytes=32,gpu_duration_ns='1234',raw_report={'path':'SECRET_SYMBOL'})
            x=dict(status='PASS_CLOSED_RAW_NCU_SYNTHETIC_FIXTURE',input_contract=contract,ROI_rows=[row],metadata_actual_controls_compared=True)
            save(directory/'ncu-result.json',x);safe=S.ncu_reference(directory,4)
            self.assertEqual(safe['ROI_rows'][0]['gpu_duration_ns'],1234);self.assertNotIn('SECRET_SYMBOL',json.dumps(safe));self.assertEqual(S.ncu_reference(directory,8)['status'],'INPUT_CONTRACT_MISMATCH')
            x['ROI_rows']=[dict(roi='D4',ncu={k:v for k,v in row.items() if k!='roi'})]
            save(directory/'ncu-result.json',x);self.assertEqual(S.ncu_reference(directory,4)['ROI_rows'],safe['ROI_rows'])
            x['metadata_actual_controls_compared']=False;save(directory/'ncu-result.json',x)
            self.assertEqual(S.ncu_reference(directory,4)['status'],'REFERENCE_NOT_CLOSED_OR_UNPAIRED')
            x['metadata_actual_controls_compared']=True;x['input_contract']=dict(contract,prompt_ids=[99,2]);save(directory/'ncu-result.json',x)
            self.assertEqual(S.ncu_reference(directory,4)['status'],'INPUT_CONTRACT_MISMATCH')

    def test_preparation_export_keeps_only_counts_and_hashes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();directory=root/'cases/qwen-p128d2'
            prepare=dict(schema='P128_PAIRED_PREPARATION_V1',status='PASS_P128_PAIRED_PREFLIGHT_NOT_EXECUTED',case_id='qwen-p128d2',model_key='qwen25_1p5b',prefill_length=128,decode_steps=2,source_pin_count=42,expected_counts=dict(kernels=7,memory_APIs=1,allocation_observations=1,phases=6),graph=dict(path='SECRET_GRAPH_PATH',sha256='b'*64),launch_resources=dict(path='SECRET_RESOURCE_PATH',sha256='c'*64),input_contract={'prompt_ids':['SECRET_PROMPT']})
            save(directory/'prepare-result.json',prepare);x=S.aggregate(root);s=json.dumps(x)
            self.assertNotIn('SECRET_',s);p=x['cases'][0]['preparation'];self.assertEqual(p['expected_counts'],prepare['expected_counts']);self.assertEqual(p['graph_sha256'],'b'*64);self.assertEqual(p['launch_resources_sha256'],'c'*64);self.assertEqual(p['source_pin_count'],42)

if __name__=='__main__':unittest.main(verbosity=2)
