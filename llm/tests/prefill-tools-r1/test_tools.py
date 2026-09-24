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
P=load('prefill_prepare_xmu');S=load('prefill_safe_status_xmu')

def save(p,x):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(x));return p

def fixture(root,prefill=64):
    native=root/'native';support=root/'support';native.mkdir();support.mkdir()
    registry=save(root/'registry.json',{});runtime=native/'runtime.py';runtime.write_text('# synthetic source only\n')
    names=P.phases(2);nodes=root/'nodes.jsonl'
    rows=[dict(kind='native_kernel',native_launch_id=i) for i in range(7)]
    rows += [dict(kind='memory_api_submission'),dict(kind='allocation_API_observation')]
    nodes.write_text(''.join(json.dumps(n)+'\n' for n in rows))
    contract=dict(model_key='qwen25_1p5b',prefill_length=prefill,decode_steps=2,batch_size=1,dtype='bfloat16',warmup_runs=1,sampling_retained=True,output_feedback=False,cuda_graph=False,prompt_ids=[7]*prefill,decode_input_ids=[8,9])
    graph=save(root/'graph.json',dict(input_contract=contract,phases=[dict(phase=p) for p in names],artifacts={'nodes':P.pin(nodes)}))
    resources=save(root/'resources.json',dict(schema='OBSERVED_LAUNCH_CARVEOUT_V1',evidence_scope='per_kernel_launch',graph_sha256=P.pin(graph)['sha256'],kernels=[dict(native_launch_id=i,observed_shared_bytes=[8192,16384,32768,65536,102400][i%5]) for i in range(7)]))
    binary=root/'build/source-cache-runner';binary.parent.mkdir();binary.write_bytes(b'\x7fELFfixture')
    sources=[]
    for name in P.KEY_BINARY_SOURCES:
        path=root/'repo'/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('// synthetic '+name);sources.append(P.pin(path))
    build=save(root/'build/job-finish.json',dict(status='PASS_PROCESS_ONLY',process={'cleanup':{'owned_descendants_empty':True}},sources=sources))
    identity=dict(model_key='qwen25_1p5b',prefill_length=prefill,decode_steps=2)
    provider=root/'provider.py';provider.write_text('# synthetic provider');contract_file=save(root/'contract.json',contract)
    native_proof=save(root/'native-case-validation.json',dict(schema='PREFILL_NATIVE_CASE_VALIDATION_V1',status='PASS_CURRENT_RAW_ABI_SASS_AND_CPP_SOURCE_EQUIVALENCE',**identity,case_id='qwen-p'+str(prefill)+'d2',graph=P.pin(graph),registry=P.pin(registry),provider=P.pin(provider),contract=P.pin(contract_file),unique_variants=1,all_bound_launches=7,negative_guard_checks=1,limitations=['synthetic fixture only']))
    qualification=save(root/'case-validation.json',dict(status='PASS_NATIVE_RUNNER_CASE_SOURCE_VALIDATION',runner_sha256=P.pin(binary)['sha256'],cases=[dict(identity,graph_sha256=P.pin(graph)['sha256'],source_qualified=True,typed_source_equivalent=True,source_validation_receipt=P.pin(native_proof))]))
    witness=save(root/'runner-evidence.json',dict(schema='PREFILL_NATIVE_RUNNER_EVIDENCE_V1',status='PASS_BUILD_AND_SOURCE_CASE_VALIDATION',binary=P.pin(binary),sources=sources,complete_compilation_dependencies=True,build_receipt=P.pin(build),case_validation=P.pin(qualification),admitted_cases=[identity]))
    return dict(schema='ADMITTED_PREFILL_CASE_V1',case_id='qwen-p'+str(prefill)+'d2',**identity,graph=str(graph),registry=str(registry),runtime=str(runtime),native_tree=str(native),support_tree=str(support),launch_resources=str(resources),runner=str(binary),runner_evidence=P.pin(witness),source_pins=[P.pin(v) for v in (graph,nodes,registry,runtime)],expected_counts=dict(kernels=7,memory_APIs=1,allocation_observations=1,phases=6))

def change_witness(a,edit):
    path=Path(a['runner_evidence']['path']);x=json.loads(path.read_text());edit(x);save(path,x);a['runner_evidence']=P.pin(path)

class PrepareTests(unittest.TestCase):
    def test_new_cases_use_pinned_frozen_producer_without_replacing_p64(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();a=fixture(root,256)
            frozen=root/'builds/p256-p512-r2/repo/llm/executor-r1/whole_stream.py'
            frozen.parent.mkdir(parents=True);frozen.write_text('# frozen synthetic producer')
            a['producer']=str(frozen)
            with self.assertRaisesRegex(ValueError,'explicit producer must be pinned'):
                P.producer_path(root,a)
            a['source_pins'].append(P.pin(frozen))
            self.assertEqual(P.producer_path(root,a),frozen)
            frozen.write_text('# changed after admission')
            with self.assertRaises(ValueError):P.producer_path(root,a)

    def test_only_three_new_prefills_and_six_full_phases(self):
        for p in (64,256,512):
            with self.subTest(prefill=p),tempfile.TemporaryDirectory() as tmp:
                a=fixture(Path(tmp).resolve(),p);paths,pins,counts=P.validate_admission(a)
                self.assertEqual(counts,dict(kernels=7,memory_APIs=1,allocation_observations=1,phases=6))
                self.assertTrue(P.binary_evidence(a,paths))
        for key,value in [('prefill_length',32),('prefill_length',128),('prefill_length',1024),('decode_steps',4),('case_id','../bad'),('graph','relative')]:
            with self.subTest(key=key,value=value),tempfile.TemporaryDirectory() as tmp:
                a=fixture(Path(tmp).resolve());a[key]=value
                with self.assertRaises(ValueError):P.validate_admission(a)

    def test_source_mutation_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            a=fixture(Path(tmp).resolve());Path(a['runtime']).write_text('changed')
            with self.assertRaisesRegex(ValueError,'pin mismatch'):P.validate_admission(a)

    def test_full_observed_resources_required(self):
        for change in ('missing','extra','duplicate','wrong_graph','range_scope'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as tmp:
                a=fixture(Path(tmp).resolve());p=Path(a['launch_resources']);x=json.loads(p.read_text())
                if change=='missing':x['kernels'].pop(0)
                elif change=='extra':x['kernels'].append(dict(native_launch_id=999,observed_shared_bytes=32768))
                elif change=='duplicate':x['kernels'].append(x['kernels'][0])
                elif change=='wrong_graph':x['graph_sha256']='0'*64
                else:x['evidence_scope']='ROI'
                save(p,x)
                with self.assertRaises(ValueError):P.validate_admission(a)

    def test_runner_requires_real_pinned_elf(self):
        for change in ('mutated_binary','not_elf','missing_dependency','build_failed','wrong_binary','wrong_shape','wrong_graph','unqualified','different_build_source','source_proof_failed'):
            with self.subTest(change=change),tempfile.TemporaryDirectory() as tmp:
                a=fixture(Path(tmp).resolve());paths,_,_=P.validate_admission(a)
                if change=='mutated_binary':Path(a['runner']).write_bytes(b'\x7fELFchanged')
                elif change=='not_elf':
                    Path(a['runner']).write_bytes(b'bad binary');change_witness(a,lambda w:w.update(binary=P.pin(a['runner'])))
                elif change=='missing_dependency':change_witness(a,lambda w:w['sources'].pop(0))
                else:
                    def edit(w):
                        if change in ('build_failed','different_build_source'):
                            p=Path(w['build_receipt']['path']);b=json.loads(p.read_text())
                            if change=='build_failed':b['status']='FAIL_PROCESS'
                            else:b['sources'][0]['sha256']='0'*64
                            save(p,b);w['build_receipt']=P.pin(p)
                        else:
                            p=Path(w['case_validation']['path']);q=json.loads(p.read_text())
                            if change=='wrong_binary':q['runner_sha256']='0'*64
                            elif change=='wrong_shape':q['cases'][0]['prefill_length']=128
                            elif change=='wrong_graph':q['cases'][0]['graph_sha256']='0'*64
                            elif change=='source_proof_failed':
                                np=Path(q['cases'][0]['source_validation_receipt']['path']);n=json.loads(np.read_text());n['status']='PARTIAL';save(np,n);q['cases'][0]['source_validation_receipt']=P.pin(np)
                            else:q['cases'][0]['typed_source_equivalent']=False
                            save(p,q);w['case_validation']=P.pin(p)
                    change_witness(a,edit)
                with self.assertRaises(ValueError):P.binary_evidence(a,paths)

    def test_r4_only_fixed_policy_cpu_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();a=fixture(root);paths,pins,_=P.validate_admission(a)
            _,_,_,specs=P.build_specs(root,a,paths,9,pins)
            self.assertEqual(list(specs),['r4']);x=specs['r4']
            self.assertIsNone(x['gpu']);self.assertEqual((x['cpu'],x['seconds'],x['rss_limit_bytes']),(9,43200,16<<30))
            self.assertEqual(x['argv'][x['argv'].index('--runner')+1],a['runner'])
            self.assertIn('--fast-gemv',x['argv']);self.assertIn('--fast-prefill',x['argv']);self.assertNotIn('--fast-down',x['argv'])
            for k,v in {'TILEGEN_ADA_L1_PROFILE':'r4','TILEGEN_ADA_REQUIRE_OBSERVED':'1','TILEGEN_EF_HIT_RATE':'288','TILEGEN_L2_DIRTY_AGE_ACCESSES':'64000000'}.items():self.assertEqual(x['environment'][k],v)
            for cpu in (-1,16,True):
                with self.assertRaises(ValueError):P.build_specs(root,a,paths,cpu,pins)

    def test_prepare_no_cache_launch_and_reference_cannot_create_case_race(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();inp=root/'inputs';inp.mkdir();a=fixture(inp);ap=save(root/'admission.json',a)
            producer=root/'repo/llm/executor-r1/whole_stream.py';producer.parent.mkdir(parents=True);producer.write_text('# fixture no execution')
            save(root/'references'/a['case_id']/'ncu-result.json',dict(status='independent fixture'))
            def preflight(cmd,**kw):
                self.assertIn('--preflight-only',cmd);self.assertNotEqual(cmd[0],a['runner'])
                self.assertEqual(kw['env']['TILEGEN_NATIVE_SUPPORT_TREE'],a['support_tree'])
                self.assertNotIn('TILEGEN_ADA_SHARED_BYTES',kw['env'])
                out=Path(cmd[cmd.index('--output')+1]);save(out/'status.json',dict(status='PASS_COMPLETE_NATIVE_INPUT_PREFLIGHT',input_preflight=dict(kernel_count=7,phases=P.phases(2)),fast_source_runtime_pins=[]))
                return types.SimpleNamespace(stdout='',stderr='',check_returncode=lambda:None)
            argv=['prepare','--root',str(root),'--admission',str(ap),'--cpu','0']
            with patch.object(P,'REMOTE_ROOT',root),patch.object(P.subprocess,'run',side_effect=preflight) as run,patch.object(sys,'argv',argv),contextlib.redirect_stdout(io.StringIO()):
                P.main();self.assertEqual(run.call_count,1)
                with self.assertRaises(ValueError):P.main()
            result=json.loads((root/'cases'/a['case_id']/'prepare-result.json').read_text())
            self.assertFalse(result['compiled']);self.assertFalse(result['launched']);self.assertEqual(len(result['specs']),1)
            self.assertEqual(result['binary'],P.pin(a['runner']));self.assertTrue((root/'references'/a['case_id']/'ncu-result.json').exists())

class StatusTests(unittest.TestCase):
    def test_pending_three_new_cases_no_reused_input_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            x=S.aggregate(Path(tmp).resolve());self.assertEqual([r['prefill_length'] for r in x['cases']],[64,256,512])
            self.assertTrue(all(c['status']=='NOT_STARTED' and c['profile']=='r4' and c['decode_steps']==2 and c['NCU']['ROI_rows'] is None for c in x['cases']))

    def test_reference_separate_path_contract_and_complete_four_rois(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();inp=root/'inputs';inp.mkdir();a=fixture(inp);case=a['case_id'];contract=json.loads(Path(a['graph']).read_text())['input_contract']
            save(root/'cases'/case/'prepare-result.json',dict(input_contract=contract))
            rows=[dict(roi=r,n=1,dram_read_bytes=100,dram_write_bytes=32,gpu_duration_ns='1234') for r in ('full','Prefill','D1','D2')]
            x=dict(status='PASS_CLOSED_RAW_NCU_SYNTHETIC_FIXTURE',input_contract=contract,ROI_rows=rows,metadata_actual_controls_compared=True,independent_ROIs_are_additive=False)
            path=root/'references'/case/'ncu-result.json';save(path,x)
            good=S.ncu_reference(root,case,64);self.assertEqual(good['ROI_rows'][0]['gpu_duration_ns'],1234)
            self.assertEqual(S.ncu_reference(root,case,256)['status'],'INPUT_CONTRACT_MISMATCH')
            x['ROI_rows']=[dict(roi=r['roi'],ncu={k:v for k,v in r.items() if k!='roi'}) for r in rows];save(path,x)
            self.assertEqual(S.ncu_reference(root,case,64)['ROI_rows'],good['ROI_rows'])
            x['ROI_rows'].pop();save(path,x);self.assertEqual(S.ncu_reference(root,case,64)['status'],'REFERENCE_ROI_SET_INCOMPLETE')
            x['ROI_rows']=rows;x['input_contract']=dict(contract,decode_input_ids=[0,0]);save(path,x)
            self.assertEqual(S.ncu_reference(root,case,64)['status'],'INPUT_CONTRACT_MISMATCH')
            x['input_contract']=contract;x['metadata_actual_controls_compared']=False;save(path,x)
            self.assertEqual(S.ncu_reference(root,case,64)['status'],'REFERENCE_NOT_CLOSED_OR_UNPAIRED')

    def test_safe_whitelist_deltas_and_source_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp).resolve();directory=root/'cases/qwen-p64d2';out=directory/'r4';secret='DO_NOT_EXPORT_0xdeadbeef_KERNEL'
            prepare=dict(schema='PREFILL_R4_PREPARATION_V1',status='PASS_PREFILL_R4_PREFLIGHT_NOT_EXECUTED',case_id='qwen-p64d2',model_key='qwen25_1p5b',prefill_length=64,decode_steps=2,profile='r4',source_pin_count=44,expected_counts=dict(kernels=7,memory_APIs=1,allocation_observations=1,phases=6),graph={'sha256':'b'*64,'path':secret},launch_resources={'sha256':'c'*64,'path':secret},binary={'sha256':'d'*64,'path':secret},runner_evidence={'sha256':'e'*64,'path':secret},input_contract={'prompt_ids':[secret]})
            save(directory/'prepare-result.json',prepare)
            save(out/'status.json',dict(status='RUNNING_COMPLETE_NATIVE_GRAPH_CACHE',error=secret,error_type='ValueError',executed_counts={'kernels':7,secret:4},source_stream=dict(bytes=42,records=4,sha256='a'*64,raw_address=secret)))
            save(out/'cache-summary.json',dict(configuration={'L1':{'profile':'r4','hash':'ALLOCATION_RELATIVE_HASH2','bytes_per_SM':102400,'symbol':secret},'L2':{'bytes':41943040,'geometry':'PAPER_ADA_L2_V1_20x1024x16','address':secret}},snapshot={'DRAM_read_bytes':128,'raw_address':123456,'L1_adapter_observation':{'L1_covered_min_line':987654}}))
            rows=[dict(type='snapshot',label='Measured/Decode1/begin',CPU_minutes_since_run_start=1,cumulative=dict(DRAM_read_bytes=128,DRAM_write_bytes=0,dirty_tail_bytes=32)),dict(type='snapshot',label='Measured/Decode1/end',CPU_minutes_since_run_start=2,cumulative=dict(DRAM_read_bytes=384,DRAM_write_bytes=32,dirty_tail_bytes=0))]
            (out/'cache-snapshots.jsonl').write_text(''.join(json.dumps(r)+'\n' for r in rows))
            x=S.aggregate(root);serialized=json.dumps(x);self.assertNotIn(secret,serialized);self.assertNotIn('987654',serialized);self.assertNotIn('123456',serialized)
            c=x['cases'][0];self.assertEqual(c['completed_phases'][0]['DRAM_read_bytes'],256);self.assertEqual(c['completed_phases'][0]['cache_CPU_minutes'],1)
            self.assertEqual(c['preparation']['binary_sha256'],'d'*64);self.assertEqual(c['preparation']['expected_counts'],prepare['expected_counts'])

if __name__=='__main__':unittest.main(verbosity=2)
